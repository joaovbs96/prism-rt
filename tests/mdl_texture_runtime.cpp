#include "../src/mdl/texture_runtime.h"
#include "../src/mdl/context.h"

#include <OpenImageIO/imagebuf.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Runtime = mdl::NativeTextureRuntime;
using Handler = Runtime::Handler;
using Base = mi::neuraylib::Texture_handler_base;

void require(bool ok, std::string const &message)
{
    if (!ok) throw std::runtime_error(message);
}

bool closeEnough(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

bool matchesSdkColor(float native, float sdk, float alpha)
{
    // MDL's EXR canvas unassociates without guarding alpha=0, so hidden
    // associated RGB becomes +/-infinity. The service deliberately returns
    // finite zero at this singularity; preserve strict parity everywhere else.
    return closeEnough(native, sdk) || (alpha == 0.0f && !std::isfinite(sdk) && native == 0.0f);
}

struct ScratchDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("prism-mdl-texture-runtime-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory() { std::filesystem::create_directories(path); }
    ~ScratchDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

struct TransactionAbort {
    mi::neuraylib::ITransaction *transaction = nullptr;
    ~TransactionAbort() { if (transaction) transaction->abort(); }
};

void setCacheDirectory(std::filesystem::path const &path)
{
#ifdef _WIN32
    _putenv_s("PRISM_CACHE_DIR", path.string().c_str());
#else
    setenv("PRISM_CACHE_DIR", path.string().c_str(), 1);
#endif
}

std::filesystem::path writePpm(std::filesystem::path const &dir)
{
    const auto path = dir / "rgb-asymmetric.ppm";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "P6\n2 2\n255\n";
    // File row zero: red, green. File row one: blue, white.
    const unsigned char pixels[] = {
        255, 0, 0, 0, 255, 0,
        0, 0, 255, 255, 255, 255};
    file.write(reinterpret_cast<char const *>(pixels), sizeof(pixels));
    require(bool(file), "failed to write RGB fixture");
    return path;
}

std::filesystem::path writePgm(std::filesystem::path const &dir)
{
    const auto path = dir / "gray-asymmetric.pgm";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "P5\n2 2\n255\n";
    const unsigned char pixels[] = {0, 64, 128, 255};
    file.write(reinterpret_cast<char const *>(pixels), sizeof(pixels));
    require(bool(file), "failed to write gray fixture");
    return path;
}

std::filesystem::path writePngRgba(std::filesystem::path const &dir)
{
    OIIO::ImageSpec spec(2, 2, 4, OIIO::TypeDesc::UINT8);
    spec.channelnames = {"R", "G", "B", "A"};
    spec.alpha_channel = 3;
    spec.attribute("oiio:UnassociatedAlpha", 1);
    OIIO::ImageBuf image(spec);
    // PNG alpha is straight. Include hidden nonzero RGB at zero alpha to
    // observe whether the SDK and the native callback preserve or clear it.
    const std::array<unsigned char, 16> pixels{
        128, 64, 32, 128, 100, 150, 200, 0,
        50, 160, 220, 64, 255, 0, 100, 255};
    require(image.set_pixels(OIIO::ROI(0, 2, 0, 2, 0, 1, 0, 4),
                             OIIO::TypeDesc::UINT8, pixels.data()),
            "failed to set RGBA PNG fixture pixels");
    const auto path = dir / "rgba-straight.png";
    require(image.write(path.string()), "failed to write RGBA PNG fixture");
    return path;
}

std::filesystem::path writeExr(std::filesystem::path const &dir, std::string name,
                               std::vector<std::string> channelNames,
                               int alphaChannel, std::vector<float> pixels,
                               int width = 2, int height = 2)
{
    const int channels = static_cast<int>(channelNames.size());
    OIIO::ImageSpec spec(width, height, channels, OIIO::TypeDesc::FLOAT);
    spec.channelnames = channelNames;
    spec.alpha_channel = alphaChannel;
    OIIO::ImageBuf image(spec);
    require(image.set_pixels(OIIO::ROI(0, width, 0, height, 0, 1, 0, channels),
                             OIIO::TypeDesc::FLOAT, pixels.data()),
            "failed to set EXR fixture pixels");
    const auto path = dir / name;
    require(image.write(path.string()), "failed to write EXR fixture: " + image.geterror());
    return path;
}

std::array<float, 4> lookup(Runtime const &runtime, float s, float t,
                           mi::neuraylib::Tex_wrap_mode wrapS,
                           mi::neuraylib::Tex_wrap_mode wrapT,
                           std::array<float, 2> cropS = {0.0f, 1.0f},
                           std::array<float, 2> cropT = {0.0f, 1.0f})
{
    std::array<float, 4> result{};
    runtime.lookupForTesting(s, t, 0, 0, 0, 0, result.data(), wrapS, wrapT,
                             cropS.data(), cropT.data());
    return result;
}

void checkTexelCallback(Runtime &runtime, int width, int height,
                        std::vector<std::array<float, 4>> const &canvasPixels)
{
    auto *handler = reinterpret_cast<Handler *>(runtime.handler());
    const auto *base = reinterpret_cast<Base const *>(handler);
    const int tile[2] = {0, 0};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int coordinate[2] = {x, y};
            float result[4]{};
            handler->vtable->m_tex_texel_float4_2d(result, base, 1, coordinate, tile, 0.0f);
            auto const &expected = canvasPixels[size_t(y) * width + x];
            for (int c = 0; c < 4; ++c)
                require(c < 3 ? matchesSdkColor(result[c], expected[c], expected[3])
                              : closeEnough(result[c], expected[c]),
                    "native texel callback differs from SDK canvas at " +
                    std::to_string(x) + "," + std::to_string(y) + " channel " +
                    std::to_string(c));
        }
    }
}

bool compareSdkCanvas(std::filesystem::path const &path, bool optional,
                      std::vector<std::array<float, 4>> *canvasPixelsOut = nullptr,
                      int *widthOut = nullptr, int *heightOut = nullptr)
{
    mdl::Context context;
    require(context.is_valid(), "cannot initialize MDL SDK: " + context.get_error());
    mi::base::Handle<mi::neuraylib::IScope> scope(context.get_database()->get_global_scope());
    require(bool(scope), "cannot access MDL global scope");
    mi::base::Handle<mi::neuraylib::ITransaction> transaction(scope->create_transaction());
    require(bool(transaction), "cannot create MDL transaction");
    TransactionAbort abort{transaction.get()};

    mi::base::Handle<mi::neuraylib::IImage> image(
        transaction->create<mi::neuraylib::IImage>("Image"));
    if (!image || image->reset_file(path.string().c_str()) != 0) {
        if (optional) return false;
        throw std::runtime_error("MDL SDK cannot load required fixture " + path.string());
    }
    mi::base::Handle<const mi::neuraylib::ICanvas> canvas(image->get_canvas(0, 0, 0));
    if (!canvas) {
        if (optional) return false;
        throw std::runtime_error("MDL SDK returned no canvas for " + path.string());
    }
    const int width = static_cast<int>(canvas->get_resolution_x());
    const int height = static_cast<int>(canvas->get_resolution_y());
    if (width <= 0 || height <= 0) {
        if (optional) return false;
        throw std::runtime_error("MDL SDK returned empty canvas for " + path.string());
    }
    mi::base::Handle<const mi::neuraylib::ITile> tile(canvas->get_tile());
    if (!tile) {
        if (optional) return false;
        throw std::runtime_error("MDL SDK returned no tile for " + path.string());
    }

    auto runtime = Runtime::CreateForTesting(path);
    require(bool(runtime), "native runtime preparation failed for " + path.string());
    std::vector<std::array<float, 4>> pixels(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float sdk[4]{};
            tile->get_pixel(static_cast<mi::Size>(x), static_cast<mi::Size>(y), sdk);
            pixels[size_t(y) * width + x] = {sdk[0], sdk[1], sdk[2], sdk[3]};
            float native[4]{};
            runtime->lookupForTesting((x + 0.5f) / width, (y + 0.5f) / height,
                                      0, 0, 0, 0, native);
            for (int c = 0; c < 4; ++c)
                require(c < 3 ? matchesSdkColor(native[c], sdk[c], sdk[3])
                              : closeEnough(native[c], sdk[c]),
                    "SDK canvas/native callback mismatch for " + path.filename().string() +
                    " at " + std::to_string(x) + "," + std::to_string(y) +
                    " channel " + std::to_string(c) + ": SDK=" +
                    std::to_string(sdk[c]) + " native=" + std::to_string(native[c]));
        }
    }
    checkTexelCallback(*runtime, width, height, pixels);
    if (canvasPixelsOut) *canvasPixelsOut = pixels;
    if (widthOut) *widthOut = width;
    if (heightOut) *heightOut = height;
    return true;
}

void checkCropAndWrapping(std::filesystem::path const &rgbPath,
                          std::vector<std::array<float, 4>> const &sdkCanvas)
{
    auto runtime = Runtime::CreateForTesting(rgbPath);
    require(bool(runtime), "cannot prepare RGB wrapping fixture");
    using W = mi::neuraylib::Tex_wrap_mode;
    const auto repeated = lookup(*runtime, 1.25f, 0.25f, W::TEX_WRAP_REPEAT, W::TEX_WRAP_CLAMP);
    const auto canonical = lookup(*runtime, 0.25f, 0.25f, W::TEX_WRAP_REPEAT, W::TEX_WRAP_CLAMP);
    for (int c = 0; c < 4; ++c)
        require(closeEnough(repeated[c], canonical[c]), "repeat wrapping differs from canonical coordinate");

    // At the periodic seam, bilinear lookup at s=0 blends the last and first
    // canvas texels in that row. Center-only repeat tests do not catch a clamp
    // accidentally being used by the native callback.
    const auto seam = lookup(*runtime, 0.0f, 0.25f, W::TEX_WRAP_REPEAT, W::TEX_WRAP_CLAMP);
    for (int c = 0; c < 4; ++c) {
        const float expected = 0.5f * (sdkCanvas[0][c] + sdkCanvas[1][c]);
        require(closeEnough(seam[c], expected),
                "repeat seam did not blend the SDK canvas edge texels");
    }

    const auto clamped = lookup(*runtime, 1.25f, 0.25f, W::TEX_WRAP_CLAMP, W::TEX_WRAP_CLAMP);
    const auto edge = lookup(*runtime, 1.0f, 0.25f, W::TEX_WRAP_CLAMP, W::TEX_WRAP_CLAMP);
    for (int c = 0; c < 4; ++c)
        require(closeEnough(clamped[c], edge[c]), "clamp wrapping differs from edge coordinate");

    const auto mirrored = lookup(*runtime, 1.25f, 0.25f, W::TEX_WRAP_MIRRORED_REPEAT,
                                 W::TEX_WRAP_CLAMP);
    const auto mirrorExpected = lookup(*runtime, 0.75f, 0.25f, W::TEX_WRAP_CLAMP,
                                       W::TEX_WRAP_CLAMP);
    for (int c = 0; c < 4; ++c)
        require(closeEnough(mirrored[c], mirrorExpected[c]), "mirrored-repeat wrapping is incorrect");

    const auto clipped = lookup(*runtime, -0.25f, 0.25f, W::TEX_WRAP_CLIP, W::TEX_WRAP_CLAMP);
    for (float channel : clipped)
        require(std::fabs(channel) < 1e-6f, "clip wrapping did not return transparent black outside domain");

}

void checkDiscreteCrop(std::filesystem::path const &path,
                       std::vector<std::array<float, 4>> const &canvasPixels,
                       int width, int height)
{
    auto runtime = Runtime::CreateForTesting(path);
    require(bool(runtime), "cannot prepare crop fixture");
    const float cropS[2] = {0.5f, 1.0f};
    const float cropT[2] = {0.5f, 1.0f};
    const auto first = lookup(*runtime, 0.25f, 0.25f,
        mi::neuraylib::TEX_WRAP_CLAMP, mi::neuraylib::TEX_WRAP_CLAMP,
        {cropS[0], cropS[1]}, {cropT[0], cropT[1]});
    const auto last = lookup(*runtime, 0.75f, 0.75f,
        mi::neuraylib::TEX_WRAP_CLAMP, mi::neuraylib::TEX_WRAP_CLAMP,
        {cropS[0], cropS[1]}, {cropT[0], cropT[1]});
    // MDL's reference maps crop bounds to integer texels before interpolation:
    // offset=floor((size-1)*min), cropResolution=floor(size*(max-min)). At
    // these chosen coordinates U/V land exactly on the first/last crop texel.
    const int cropWidth = std::max(1, int(width * (cropS[1] - cropS[0])));
    const int cropHeight = std::max(1, int(height * (cropT[1] - cropT[0])));
    const int offsetX = int((width - 1) * cropS[0]);
    const int offsetY = int((height - 1) * cropT[0]);
    const int firstX = offsetX;
    const int firstY = offsetY;
    const int lastX = offsetX + cropWidth - 1;
    const int lastY = offsetY + cropHeight - 1;
    auto const &expectedFirst = canvasPixels[size_t(firstY) * width + firstX];
    auto const &expectedLast = canvasPixels[size_t(lastY) * width + lastX];
    for (int c = 0; c < 4; ++c) {
        require(closeEnough(first[c], expectedFirst[c]),
                "partial crop did not use the SDK's integer texel offset/resolution");
        require(closeEnough(last[c], expectedLast[c]),
                "partial crop did not reach the SDK's last cropped texel");
    }
}

std::array<float,4> lookup3d(Runtime const& runtime,float x,float y,float z,
    mi::neuraylib::Tex_wrap_mode wrap=mi::neuraylib::TEX_WRAP_CLAMP,
    std::array<float,2> cropX={0,1},std::array<float,2> cropY={0,1},
    std::array<float,2> cropZ={0,1})
{
    std::array<float,4> result{};
    runtime.lookup3dForTesting(x,y,z,result.data(),wrap,wrap,wrap,
        cropX.data(),cropY.data(),cropZ.data());
    return result;
}

void checkSyntheticDfData()
{
    std::vector<float> values;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
        const float v=float(4*z+2*y+x);
        values.insert(values.end(),{v,v+10.0f,v+20.0f});
    }
    auto runtime=Runtime::CreateDfDataForTesting(
        mi::neuraylib::DFK_SIMPLE_GLOSSY_MULTISCATTER,2,2,2,"Float32<3>",values.data());
    require(bool(runtime),"cannot create synthetic BSDF data runtime");
    auto* handler=reinterpret_cast<Handler*>(runtime->handler());
    auto const* base=reinterpret_cast<Base const*>(handler);
    require(!handler->vtable->m_tex_texture_isvalid(base,0),"invalid prefix slot became valid");
    require(handler->vtable->m_tex_texture_isvalid(base,1),"BSDF table slot is invalid");
    int resolution[3]{};handler->vtable->m_tex_resolution_3d(resolution,base,1,0);
    require(resolution[0]==2&&resolution[1]==2&&resolution[2]==2,
            "BSDF table resolution callback mismatch");
    int texelCoordinate[3]={1,0,1};float texel[4]{};
    handler->vtable->m_tex_texel_float4_3d(texel,base,1,texelCoordinate,0);
    require(closeEnough(texel[0],5)&&closeEnough(texel[1],15)&&
            closeEnough(texel[2],25)&&closeEnough(texel[3],1),
            "BSDF table texel callback mismatch");
    const auto center=lookup3d(*runtime,.5f,.5f,.5f);
    require(closeEnough(center[0],3.5f)&&closeEnough(center[1],13.5f)&&
            closeEnough(center[2],23.5f)&&closeEnough(center[3],1),
            "BSDF table trilinear interpolation mismatch");
    const auto cropped=lookup3d(*runtime,.5f,.25f,.25f,
        mi::neuraylib::TEX_WRAP_CLAMP,{.5f,1.0f},{0,1},{0,1});
    const auto croppedTexel=lookup3d(*runtime,.25f,.25f,.25f,
                                     mi::neuraylib::TEX_WRAP_CLAMP);
    for(int c=0;c<4;++c)require(closeEnough(cropped[c],croppedTexel[c]),
        "BSDF table integer crop mapping mismatch");
    const auto repeat=lookup3d(*runtime,1.25f,.25f,.25f,mi::neuraylib::TEX_WRAP_REPEAT);
    const auto canonical=lookup3d(*runtime,.25f,.25f,.25f,mi::neuraylib::TEX_WRAP_REPEAT);
    for(int c=0;c<4;++c)require(closeEnough(repeat[c],canonical[c]),
        "BSDF table repeat wrapping mismatch");
    const auto mirror=lookup3d(*runtime,1.25f,.25f,.25f,
                               mi::neuraylib::TEX_WRAP_MIRRORED_REPEAT);
    const auto mirroredExpected=lookup3d(*runtime,.75f,.25f,.25f,
                                         mi::neuraylib::TEX_WRAP_CLAMP);
    for(int c=0;c<4;++c)require(closeEnough(mirror[c],mirroredExpected[c]),
        "BSDF table mirrored wrapping mismatch");
    const auto clipped=lookup3d(*runtime,1.0f,.5f,.5f,mi::neuraylib::TEX_WRAP_CLIP);
    for(float channel:clipped)require(channel==0.0f,"BSDF table clip did not return black");
    require(bool(Runtime::CreateDfDataForTesting(mi::neuraylib::DFK_INVALID,2,2,2,
                                                 "Float32<3>",values.data())),
            "unclassified SDK BSDF payload with a valid layout was rejected");
    require(!Runtime::CreateDfDataForTesting(
        mi::neuraylib::DFK_SIMPLE_GLOSSY_MULTISCATTER,2,2,2,"Color",values.data()),
            "unsupported BSDF data pixel type was accepted");
}

void checkSdkDfData()
{
    mdl::Context context;
    require(context.is_valid(),"cannot initialize MDL SDK for BSDF tables: "+context.get_error());
    mi::base::Handle<mi::neuraylib::IMdl_backend_api> api(
        context.get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());
    require(bool(api),"cannot access MDL backend API for BSDF tables");
    const mi::neuraylib::Df_data_kind kinds[]={
        mi::neuraylib::DFK_SIMPLE_GLOSSY_MULTISCATTER,
        mi::neuraylib::DFK_BACKSCATTERING_GLOSSY_MULTISCATTER,
        mi::neuraylib::DFK_BECKMANN_SMITH_MULTISCATTER,
        mi::neuraylib::DFK_BECKMANN_VC_MULTISCATTER,
        mi::neuraylib::DFK_GGX_SMITH_MULTISCATTER,
        mi::neuraylib::DFK_GGX_VC_MULTISCATTER,
        mi::neuraylib::DFK_WARD_GEISLER_MORODER_MULTISCATTER,
        mi::neuraylib::DFK_SHEEN_MULTISCATTER,
        mi::neuraylib::DFK_MICROFLAKE_SHEEN_GENERAL,
        mi::neuraylib::DFK_MICROFLAKE_SHEEN_MULTISCATTER};
    size_t checked=0;
    for(auto kind:kinds) {
        mi::Size width=0,height=0,depth=0;const char* pixelType=nullptr;
        const float* source=api->get_df_data_texture(kind,width,height,depth,pixelType);
        if(!source)continue;
        auto runtime=Runtime::CreateDfDataForTesting(
            kind,width,height,depth,pixelType,source);
        require(bool(runtime),"native runtime rejected an SDK BSDF data table");
        auto* handler=reinterpret_cast<Handler*>(runtime->handler());
        auto const* base=reinterpret_cast<Base const*>(handler);
        int resolution[3]{};handler->vtable->m_tex_resolution_3d(resolution,base,1,0);
        require(resolution[0]==int(width)&&resolution[1]==int(height)&&resolution[2]==int(depth),
                "SDK BSDF table resolution changed during copy");
        const int coordinates[3]={int(width/2),int(height/2),int(depth/2)};
        float texel[4]{};handler->vtable->m_tex_texel_float4_3d(
            texel,base,1,coordinates,0);
        const size_t components=std::string(pixelType)=="Float32"?1:3;
        const size_t offset=((size_t(coordinates[2])*height+size_t(coordinates[1]))*width+
                             size_t(coordinates[0]))*components;
        for(size_t c=0;c<3;++c) {
            const float expected=source[offset+(components==1?0:c)];
            require(closeEnough(texel[c],expected),"SDK BSDF table texel changed during copy");
        }
        const auto center=lookup3d(*runtime,(coordinates[0]+.5f)/float(width),
            (coordinates[1]+.5f)/float(height),(coordinates[2]+.5f)/float(depth));
        for(int c=0;c<4;++c)require(closeEnough(center[c],texel[c]),
            "BSDF center lookup differs from exact SDK table texel");
        ++checked;
    }
    require(checked>0,"installed MDL SDK exposed no BSDF data tables");
}

// The BSDF table lookup as it was before its fast path (texture_runtime.cpp):
// eight bounds-checked corners, four channels blended. The fast path must
// return the same bits.
namespace reference {
bool CropAxis(float const crop[2],size_t size,size_t* offset,size_t* resolution)
{
    if(!crop || !std::isfinite(crop[0]) || !std::isfinite(crop[1]) ||
       crop[0]<0.0f || crop[1]>1.0f || crop[1]<crop[0] || !size)return false;
    *offset=static_cast<size_t>(float(size-1)*crop[0]);
    *resolution=std::max<size_t>(static_cast<size_t>(float(size)*(crop[1]-crop[0])),1);
    if(*offset>=size)*offset=size-1;
    *resolution=std::min(*resolution,size-*offset);
    return true;
}
bool RemapTexel(float coordinate,size_t size,mi::neuraylib::Tex_wrap_mode mode,
                size_t offset,int64_t* lower,int64_t* upper,float* fraction)
{
    using M=mi::neuraylib::Tex_wrap_mode;
    if(!std::isfinite(coordinate) || !size)return false;
    if(mode==M::TEX_WRAP_CLIP && (coordinate<0.0f || coordinate>=1.0f))return false;
    const double tex=double(coordinate)*double(size)-0.5;
    if(tex<double(std::numeric_limits<int64_t>::min()+2) ||
       tex>double(std::numeric_limits<int64_t>::max()-2))return false;
    const auto base=static_cast<int64_t>(std::floor(tex));
    *fraction=static_cast<float>(tex-std::floor(tex));
    auto map=[&](int64_t value)->int64_t {
        const int64_t n=static_cast<int64_t>(size);
        if(mode==M::TEX_WRAP_CLAMP || mode==M::TEX_WRAP_CLIP)
            return std::clamp<int64_t>(value,0,n-1)+static_cast<int64_t>(offset);
        int64_t cell=value/n;
        int64_t local=value%n;
        if(local<0){local+=n;--cell;}
        if(mode==M::TEX_WRAP_MIRRORED_REPEAT && (std::abs(cell)%2)==1)
            local=n-1-local;
        return local+static_cast<int64_t>(offset);
    };
    *lower=map(base);*upper=map(base+1);return true;
}
void LoadDfTexel(const float* values,size_t width,size_t height,size_t depth,
                 size_t components,int64_t x,int64_t y,int64_t z,float out[4])
{
    out[0]=out[1]=out[2]=0.0f;out[3]=1.0f;
    if(x<0||y<0||z<0||size_t(x)>=width||size_t(y)>=height||size_t(z)>=depth)return;
    const size_t index=((size_t(z)*height+size_t(y))*width+size_t(x))*components;
    if(components==1)out[0]=out[1]=out[2]=values[index];
    else {out[0]=values[index];out[1]=values[index+1];out[2]=values[index+2];}
}
void Lookup(const float* values,size_t width,size_t height,size_t depth,size_t components,
            float const coord[3],mi::neuraylib::Tex_wrap_mode wrap,float const crop[3][2],float out[4])
{
    out[0]=out[1]=out[2]=out[3]=0.0f;
    size_t ox=0,oy=0,oz=0,nx=0,ny=0,nz=0;
    if(!CropAxis(crop[0],width,&ox,&nx) || !CropAxis(crop[1],height,&oy,&ny) ||
       !CropAxis(crop[2],depth,&oz,&nz))return;
    int64_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;float fx=0,fy=0,fz=0;
    if(!RemapTexel(coord[0],nx,wrap,ox,&x0,&x1,&fx) || !RemapTexel(coord[1],ny,wrap,oy,&y0,&y1,&fy) ||
       !RemapTexel(coord[2],nz,wrap,oz,&z0,&z1,&fz))return;
    float corners[8][4];
    const int64_t xs[2]={x0,x1},ys[2]={y0,y1},zs[2]={z0,z1};
    for(int i=0;i<8;++i)
        LoadDfTexel(values,width,height,depth,components,xs[i&1],ys[(i>>1)&1],zs[i>>2],corners[i]);
    auto lerp=[](float a,float b,float t){return (1.0f-t)*a+t*b;};
    for(int c=0;c<4;++c) {
        const float a=lerp(lerp(corners[0][c],corners[1][c],fx),lerp(corners[2][c],corners[3][c],fx),fy);
        const float b=lerp(lerp(corners[4][c],corners[5][c],fx),lerp(corners[6][c],corners[7][c],fx),fy);
        out[c]=lerp(a,b,fz);
    }
}
} // namespace reference

// The fast BSDF table lookup returns the reference's bits for scalar and RGB
// tables, every wrap mode, full and partial crops, inside and outside [0, 1].
void checkDfLookupMatchesReference()
{
    mdl::Context context;
    require(context.is_valid(),"cannot initialize MDL SDK for BSDF tables: "+context.get_error());
    mi::base::Handle<mi::neuraylib::IMdl_backend_api> api(
        context.get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());
    require(bool(api),"cannot access MDL backend API for BSDF tables");
    const mi::neuraylib::Tex_wrap_mode wraps[]={mi::neuraylib::TEX_WRAP_CLAMP,mi::neuraylib::TEX_WRAP_REPEAT,
        mi::neuraylib::TEX_WRAP_MIRRORED_REPEAT,mi::neuraylib::TEX_WRAP_CLIP};
    const float crops[3][3][2]={{{0,1},{0,1},{0,1}},{{.25f,.75f},{0,1},{.5f,1}},{{0,.3f},{.1f,.9f},{.6f,.6f}}};
    uint32_t state=12345u;
    auto next=[&]{state=state*1664525u+1013904223u;return float(state>>8)/float(1u<<24);};
    size_t checked=0,scalar=0;
    for(auto kind:{mi::neuraylib::DFK_SIMPLE_GLOSSY_MULTISCATTER,mi::neuraylib::DFK_GGX_SMITH_MULTISCATTER,
                   mi::neuraylib::DFK_SHEEN_MULTISCATTER,mi::neuraylib::DFK_MICROFLAKE_SHEEN_GENERAL,
                   mi::neuraylib::DFK_MICROFLAKE_SHEEN_MULTISCATTER}) {
        mi::Size width=0,height=0,depth=0;const char* pixelType=nullptr;
        const float* source=api->get_df_data_texture(kind,width,height,depth,pixelType);
        if(!source)continue;
        auto runtime=Runtime::CreateDfDataForTesting(kind,width,height,depth,pixelType,source);
        require(bool(runtime),"native runtime rejected an SDK BSDF data table");
        auto* handler=reinterpret_cast<Handler*>(runtime->handler());
        auto const* base=reinterpret_cast<Base const*>(handler);
        const size_t components=std::string(pixelType)=="Float32"?1:3;
        scalar+=components==1;
        for(auto wrap:wraps)
            for(auto const& crop:crops)
                for(int i=0;i<2000;++i) {
                    const float coord[3]={next()*1.6f-.3f,next()*1.6f-.3f,next()*1.6f-.3f};
                    float expected[4],fast4[4],fast3[3];
                    reference::Lookup(source,width,height,depth,components,coord,wrap,crop,expected);
                    handler->vtable->m_tex_lookup_float4_3d(fast4,base,1,coord,wrap,wrap,wrap,
                                                            crop[0],crop[1],crop[2],0.0f);
                    handler->vtable->m_tex_lookup_float3_3d(fast3,base,1,coord,wrap,wrap,wrap,
                                                            crop[0],crop[1],crop[2],0.0f);
                    require(std::memcmp(expected,fast4,sizeof(fast4))==0,
                            "BSDF table float4 lookup differs from the reference");
                    require(std::memcmp(expected,fast3,sizeof(fast3))==0,
                            "BSDF table float3 lookup differs from the reference");
                    ++checked;
                }
    }
    require(checked>0 && scalar>0,"installed MDL SDK exposed no scalar and RGB BSDF data tables");
}
}

int main()
{
    try {
        ScratchDirectory scratch;
        setCacheDirectory(scratch.path / "texture-cache");
        const auto rgb = writePpm(scratch.path);
        const auto gray = writePgm(scratch.path);
        const auto pngRgba = writePngRgba(scratch.path);
        const auto rgba = writeExr(scratch.path, "rgba.exr", {"R", "G", "B", "A"}, 3,
            {0.9f, 0.1f, 0.2f, 0.3f, 0.4f, 0.8f, 0.2f, 0.6f,
             0.1f, 0.3f, 0.9f, 0.0f, 0.7f, 0.5f, 0.3f, 1.0f});
        const auto grayAlpha = writeExr(scratch.path, "gray-alpha.exr", {"Y", "A"}, 1,
            {0.15f, 0.25f, 0.4f, 0.5f, 0.65f, 0.75f, 0.9f, 1.0f});
        std::vector<float> cropPixels(4 * 4 * 3);
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            const size_t i = (size_t(y) * 4 + x) * 3;
            cropPixels[i] = float(x + 1) / 5.0f;
            cropPixels[i + 1] = float(y + 1) / 5.0f;
            cropPixels[i + 2] = float(y * 4 + x + 1) / 17.0f;
        }
        const auto cropPath = writeExr(scratch.path, "crop-4x4.exr", {"R", "G", "B"}, -1,
                                       std::move(cropPixels), 4, 4);

        std::vector<std::array<float, 4>> rgbCanvas;
        require(compareSdkCanvas(rgb, false, &rgbCanvas), "RGB SDK comparison failed");
        require(compareSdkCanvas(gray, false), "gray SDK comparison failed");
        require(compareSdkCanvas(rgba, false), "RGBA SDK comparison failed");
        require(compareSdkCanvas(pngRgba, false), "RGBA PNG SDK comparison failed");
        const bool grayAlphaSupported = compareSdkCanvas(grayAlpha, true);
        if (grayAlphaSupported)
            std::cout << "MDL SDK supports gray-alpha fixture; comparison passed\n";
        else
            std::cout << "MDL SDK did not load gray-alpha fixture; optional case skipped\n";
        checkCropAndWrapping(rgb, rgbCanvas);
        std::vector<std::array<float, 4>> cropCanvas;
        int cropWidth = 0, cropHeight = 0;
        require(compareSdkCanvas(cropPath, false, &cropCanvas, &cropWidth, &cropHeight),
                "crop source SDK comparison failed");
        checkDiscreteCrop(cropPath, cropCanvas, cropWidth, cropHeight);
        checkSyntheticDfData();
        checkSdkDfData();
        checkDfLookupMatchesReference();

        auto runtime = Runtime::CreateForTesting(rgba);
        require(bool(runtime), "cannot prepare filtered lookup fixture");
        float filtered[4]{};
        runtime->lookupForTesting(0.5f, 0.5f, 0.8f, 0, 0, 0.8f, filtered);
        for (float channel : filtered)
            require(std::isfinite(channel), "filtered lookup returned a nonfinite channel");
        std::cout << "MDL SDK canvas/native RGB, gray, RGBA, texel, crop and wrap checks passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "mdl_texture_runtime: " << error.what() << '\n';
        return 1;
    }
}
