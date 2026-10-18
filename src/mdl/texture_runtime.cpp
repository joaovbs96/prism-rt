#include "texture_runtime.h"

#include <mi/neuraylib/iimage.h>
#include <mi/neuraylib/itexture.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace mdl {
namespace {
using Base = mi::neuraylib::Texture_handler_base;
using Deriv2 = mi::neuraylib::tct_deriv_float2;

NativeTextureRuntime const *Self(Base const *base)
{
    auto const *handler=reinterpret_cast<NativeTextureRuntime::Handler const *>(base);
    return handler ? handler->owner : nullptr;
}

prism::TextureWrap Wrap(mi::neuraylib::Tex_wrap_mode mode)
{
    using M = mi::neuraylib::Tex_wrap_mode;
    switch (mode) {
    case M::TEX_WRAP_CLAMP: return prism::TextureWrap::Clamp;
    case M::TEX_WRAP_REPEAT: return prism::TextureWrap::Periodic;
    case M::TEX_WRAP_MIRRORED_REPEAT: return prism::TextureWrap::Mirror;
    // Clip rejects outside coordinates before lookup; valid edge coordinates
    // still clamp their interpolation neighbors, as in the MDL runtime.
    case M::TEX_WRAP_CLIP: return prism::TextureWrap::Clamp;
    default: return prism::TextureWrap::Default;
    }
}

// MDL applies wrapping to the lookup coordinate before mapping it into the
// cropped interval. Doing this explicitly also keeps UDIM tile selection based
// on the uncropped coordinate, as required by the native texture ABI.
bool Remap(float &x, float &dx, float &dy, mi::neuraylib::Tex_wrap_mode mode)
{
    using M = mi::neuraylib::Tex_wrap_mode;
    if (mode == M::TEX_WRAP_CLIP && (x < 0.0f || x > 1.0f)) return false;
    if (mode == M::TEX_WRAP_CLAMP || mode == M::TEX_WRAP_CLIP) {
        if (x <= 0.0f || x >= 1.0f) dx=dy=0.0f;
        x=std::clamp(x,0.0f,1.0f);
    } else if (mode == M::TEX_WRAP_MIRRORED_REPEAT) {
        const float cell=std::floor(x);
        x-=cell;
        if (std::fmod(std::abs(cell),2.0f)!=0) { x=1.0f-x; dx=-dx; dy=-dy; }
    } else { // repeat and the ABI's default wrap mode
        x-=std::floor(x);
    }
    return true;
}

// MDL expands scalar images to RGB and supplies opaque alpha for images
// without an alpha channel. OIIO instead fills absent lookup channels with 0.
void ExpandChannels(int channels, float out[4])
{
    if (channels == 1) { out[1]=out[2]=out[0]; out[3]=1.0f; }
    else if (channels == 2) { out[3]=out[1]; out[1]=out[2]=out[0]; }
    else if (channels == 3) out[3]=1.0f;
}

bool CheckedMultiply(size_t a,size_t b,size_t* result)
{
    if(a && b>std::numeric_limits<size_t>::max()/a)return false;
    *result=a*b;return true;
}

uint64_t HashFloats(const float* values,size_t count)
{
    uint64_t hash=1469598103934665603ull;
    auto bytes=reinterpret_cast<const unsigned char*>(values);
    for(size_t i=0;i<count*sizeof(float);++i){hash^=bytes[i];hash*=1099511628211ull;}
    return hash;
}

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
}

NativeTextureRuntime::NativeTextureRuntime()
    : m_service(prism::TextureService::Acquire())
{ m_handler.vtable = &Vtable(); m_handler.owner=this; }

std::shared_ptr<const NativeTextureRuntime::DfData> NativeTextureRuntime::CopyDfData(
    mi::neuraylib::Df_data_kind kind,mi::Size width,mi::Size height,mi::Size depth,
    const char* pixelType,const float* data,std::string* error)
{
    auto fail=[&](std::string message)->std::shared_ptr<const DfData>{
        if(error)*error=std::move(message);return {};
    };
    // MDL SDK 2025.0.3 can classify compiler-injected MaterialX microflake
    // sheen tables as DFK_INVALID while still providing a complete shape-5
    // payload. The callback ABI treats these resources as generic 3D textures,
    // so capability is determined by the concrete layout validated below.
    if(!width||!height||!depth||!data||!pixelType)
        return fail("empty BSDF data table");
    size_t components=0;
    if(std::strcmp(pixelType,"Float32")==0)components=1;
    else if(std::strcmp(pixelType,"Float32<3>")==0)components=3;
    else return fail("unsupported BSDF data pixel type: "+std::string(pixelType));
    if(width>size_t(std::numeric_limits<int>::max()) ||
       height>size_t(std::numeric_limits<int>::max()) ||
       depth>size_t(std::numeric_limits<int>::max()))
        return fail("BSDF data extent exceeds native callback limits");
    size_t texels=0,count=0;
    if(!CheckedMultiply(size_t(width),size_t(height),&texels) ||
       !CheckedMultiply(texels,size_t(depth),&texels) ||
       !CheckedMultiply(texels,components,&count) ||
       count>std::vector<float>().max_size())
        return fail("BSDF data extent overflows address space");
    const uint64_t digest=HashFloats(data,count);
    std::ostringstream keyBuilder;
    keyBuilder<<unsigned(kind)<<':'<<width<<':'<<height<<':'<<depth<<':'
              <<pixelType<<':'<<std::hex<<digest;
    const std::string key=keyBuilder.str();
    static std::mutex mutex;
    static std::unordered_map<std::string,std::weak_ptr<const DfData>> tables;
    std::lock_guard lock(mutex);
    if(auto found=tables.find(key);found!=tables.end())
        if(auto existing=found->second.lock())
            if(existing->values.size()==count &&
               std::equal(existing->values.begin(),existing->values.end(),data))return existing;
    auto copied=std::make_shared<DfData>();
    copied->kind=kind;copied->width=size_t(width);copied->height=size_t(height);
    copied->depth=size_t(depth);copied->components=components;copied->pixelType=pixelType;
    copied->values.assign(data,data+count);
    tables[key]=copied;
    return copied;
}

std::shared_ptr<NativeTextureRuntime> NativeTextureRuntime::CreateForTesting(
    const std::filesystem::path& image)
{
    auto result=std::shared_ptr<NativeTextureRuntime>(new NativeTextureRuntime);
    result->m_textures.resize(2);
    prism::TexturePrepareOptions options;
    options.unassociateAlpha=true;
    auto resource=result->m_service->prepare(image,options);
    if(!resource) return {};
    result->m_textures[1].tiles.emplace(std::pair{0,0},std::move(resource));
    return result;
}

std::shared_ptr<NativeTextureRuntime> NativeTextureRuntime::CreateDfDataForTesting(
    mi::neuraylib::Df_data_kind kind,mi::Size width,mi::Size height,mi::Size depth,
    const char* pixelType,const float* data)
{
    std::string error;
    auto table=CopyDfData(kind,width,height,depth,pixelType,data,&error);
    if(!table)return {};
    auto result=std::shared_ptr<NativeTextureRuntime>(new NativeTextureRuntime);
    result->m_textures.resize(2);result->m_textures[1].dfData=std::move(table);
    return result;
}

void NativeTextureRuntime::lookupForTesting(float s,float t,float dsdx,float dtdx,
    float dsdy,float dtdy,float out[4]) const
{
    const float crop[2]={0,1};
    lookupForTesting(s,t,dsdx,dtdx,dsdy,dtdy,out,
        mi::neuraylib::TEX_WRAP_CLAMP,mi::neuraylib::TEX_WRAP_CLAMP,crop,crop);
}

void NativeTextureRuntime::lookup3dForTesting(float s,float t,float r,float out[4],
    mi::neuraylib::Tex_wrap_mode wrapS,mi::neuraylib::Tex_wrap_mode wrapT,
    mi::neuraylib::Tex_wrap_mode wrapR,float const cropS[2],float const cropT[2],
    float const cropR[2]) const
{
    const float coord[3]={s,t,r};
    Vtable().m_tex_lookup_float4_3d(out,
        reinterpret_cast<Base const*>(&m_handler),1,coord,wrapS,wrapT,wrapR,
        cropS,cropT,cropR,0.0f);
}

void NativeTextureRuntime::lookupForTesting(float s,float t,float dsdx,float dtdx,
    float dsdy,float dtdy,float out[4],mi::neuraylib::Tex_wrap_mode wrapS,
    mi::neuraylib::Tex_wrap_mode wrapT,float const cropS[2],float const cropT[2]) const
{
    Deriv2 coord{}; coord.val={s,t}; coord.dx={dsdx,dtdx}; coord.dy={dsdy,dtdy};
    Vtable().m_tex_lookup_float4_2d(out,
        reinterpret_cast<Base const*>(&m_handler),1,&coord,
        wrapS,wrapT,cropS,cropT,0);
}

NativeTextureRuntime::ResourceView NativeTextureRuntime::resource(size_t index) const
{
    ResourceView view;
    if (index >= m_textures.size()) return view;
    auto const& entry = m_textures[index];
    view.tiled = entry.tiled;
    view.tiles.assign(entry.tiles.begin(), entry.tiles.end());
    if (entry.dfData) {
        view.owner = entry.dfData;
        view.values = entry.dfData->values.data();
        view.width = entry.dfData->width;
        view.height = entry.dfData->height;
        view.depth = entry.dfData->depth;
        view.components = entry.dfData->components;
    }
    return view;
}

bool NativeTextureRuntime::PrepareDatabaseTexture(const char* dbName,
    mi::neuraylib::ITransaction* transaction, ResourceView* result, std::string* error)
{
    if (!dbName || !transaction || !result) {
        if (error) *error = "invalid texture request";
        return false;
    }
    mi::base::Handle<const mi::neuraylib::ITexture> texture(
        transaction->access<const mi::neuraylib::ITexture>(dbName));
    mi::base::Handle<const mi::neuraylib::IImage> image(texture
        ? transaction->access<const mi::neuraylib::IImage>(texture->get_image()) : nullptr);
    Texture entry;
    if (!prepareImage(texture.get(), image.get(), prism::TextureService::Acquire(),
                      entry, error)) return false;
    *result = {};
    result->tiled = entry.tiled;
    result->tiles.assign(entry.tiles.begin(), entry.tiles.end());
    return true;
}

bool NativeTextureRuntime::IsTexcoordPrimvarName(const char* name)
{
    if(!name) return false;
    static const char* const names[]={"st","uv","UV","st0","uv0","UV0","map1","UVMap","primvars:st"};
    for(const char* candidate:names) if(std::strcmp(name,candidate)==0) return true;
    return false;
}

std::shared_ptr<NativeTextureRuntime> NativeTextureRuntime::clone() const
{
    auto copy=std::shared_ptr<NativeTextureRuntime>(new NativeTextureRuntime);
    copy->m_textures=m_textures;
    copy->m_texcoordSceneData=m_texcoordSceneData;
    return copy;
}

std::shared_ptr<const prism::TextureResource> NativeTextureRuntime::croppedResource(
    std::shared_ptr<const prism::TextureResource> const& source,
    float const cropS[2],float const cropT[2]) const
{
    if (cropS[0]==0 && cropS[1]==1 && cropT[0]==0 && cropT[1]==1) return source;
    if (!std::isfinite(cropS[0]) || !std::isfinite(cropS[1]) ||
        !std::isfinite(cropT[0]) || !std::isfinite(cropT[1])) return {};
    // MDL defines cropping as an integer texel rectangle, not a continuous
    // coordinate scale. Generate the crop's own mips so border filtering does
    // not pull colors from the excluded part of the source image.
    const float s0=std::clamp(cropS[0],0.0f,1.0f), s1=std::clamp(cropS[1],s0,1.0f);
    const float t0=std::clamp(cropT[0],0.0f,1.0f), t1=std::clamp(cropT[1],t0,1.0f);
    const int x=int((source->width()-1)*s0), bottom=int((source->height()-1)*t0);
    const int width=std::min(source->width()-x,std::max(1,int(source->width()*(s1-s0))));
    const int height=std::min(source->height()-bottom,std::max(1,int(source->height()*(t1-t0))));
    const int y=source->height()-bottom-height;
    const CropKey key{source.get(),x,y,width,height};
    std::lock_guard lock(m_cropMutex);
    if (auto found=m_crops.find(key);found!=m_crops.end()) {
        found->second.lastUse=++m_cropClock;
        return found->second.resource;
    }
    const int channels=source->channels();
    std::vector<float> pixels(size_t(width)*height*channels);
    if(!m_service->readPixels(*source,x,y,x+width,y+height,0,channels,pixels.data())) return {};
    OIIO::ImageSpec spec(width,height,channels,OIIO::TypeDesc::FLOAT);
    if(channels==2 || channels==4) spec.alpha_channel=channels-1;
    OIIO::ImageBuf image(spec);
    if(!image.set_pixels(OIIO::ROI(0,width,0,height,0,1,0,channels),
                         OIIO::TypeDesc::FLOAT,pixels.data())) return {};
    auto prepared=m_service->prepare(image);
    // Crop parameters can be varying expressions. Bound retained handles;
    // disk entries and OIIO tiles remain subject to the shared cache budgets.
    if(m_crops.size()>=64) {
        auto oldest=std::min_element(m_crops.begin(),m_crops.end(),
            [](auto const& a,auto const& b){return a.second.lastUse<b.second.lastUse;});
        m_crops.erase(oldest);
    }
    if(prepared) m_crops.emplace(key,CropEntry{prepared,++m_cropClock});
    return prepared;
}

bool NativeTextureRuntime::prepareImage(const mi::neuraylib::ITexture* texture,
    const mi::neuraylib::IImage* image,
    const std::shared_ptr<prism::TextureService>& service,
    Texture& entry, std::string* fallbackReason)
{
    auto unsupported = [&](std::string reason) {
        if (fallbackReason) *fallbackReason = std::move(reason);
        return false;
    };
    if (!texture) return unsupported("missing texture database entry");
    if (!image) return unsupported("missing image database entry");
    if (image->is_animated()) return unsupported("animated texture");
    entry.tiled = image->is_uvtile();
    const mi::Size count = image->get_frame_length(0);
    try {
        for (mi::Size tile = 0; tile < count; ++tile) {
            int u = 0, v = 0;
            if (image->get_uvtile_uv(0, tile, u, v) != 0)
                return unsupported("invalid UDIM tile coordinates");
            const char* filename = image->get_filename(0, tile);
            if (!filename || !*filename)
                return unsupported("texture without filesystem backing");
            prism::TexturePrepareOptions options;
            options.gamma = texture->get_effective_gamma(0, tile);
            options.unassociateAlpha = true;
            auto prepared = service->prepare(filename, options);
            if (!prepared)
                return unsupported("texture preparation failed: " + std::string(filename));
            entry.tiles.emplace(std::pair{u, v}, std::move(prepared));
        }
    } catch (const std::exception& error) {
        return unsupported(error.what());
    }
    if (entry.tiles.empty()) return unsupported("texture has no image tiles");
    return true;
}

bool NativeTextureRuntime::PreflightArgumentResource(
    const mi::neuraylib::IValue_resource* resource,
    mi::neuraylib::ITransaction* transaction,
    ArgumentResourcePreflight& retained,
    std::string* fallbackReason)
{
    mi::base::Handle<const mi::neuraylib::IValue_texture> value(
        resource ? resource->get_interface<const mi::neuraylib::IValue_texture>() : nullptr);
    if (!value || !transaction) {
        if (fallbackReason) *fallbackReason = "invalid texture argument";
        return false;
    }
    const char* valueName = value->get_value();
    if (!valueName || !*valueName) return true; // MDL invalid resource index 0.
    if (value->get_selector() && *value->get_selector()) {
        if (fallbackReason) *fallbackReason = "texture selectors require SDK resource handler";
        return false;
    }
    mi::base::Handle<const mi::neuraylib::IType_texture> type(value->get_type());
    if (!type || type->get_shape() != mi::neuraylib::IType_texture::TS_2D) {
        if (fallbackReason) *fallbackReason = "authored non-2D texture requires SDK resource handler";
        return false;
    }
    const char *name=valueName;
    mi::base::Handle<const mi::neuraylib::ITexture> texture(
        name ? transaction->access<const mi::neuraylib::ITexture>(name) : nullptr);
    mi::base::Handle<const mi::neuraylib::IImage> image(
        texture ? transaction->access<const mi::neuraylib::IImage>(texture->get_image()) : nullptr);
    Texture entry;
    auto service = prism::TextureService::Acquire();
    if (!prepareImage(texture.get(), image.get(), service, entry, fallbackReason)) return false;
    for (const auto& [tile, prepared] : entry.tiles) retained.retained.push_back(prepared);
    return true;
}

mi::Uint32 NativeTextureRuntime::registerTexture(
    const mi::neuraylib::IValue_resource *resource,
    mi::neuraylib::ITransaction *transaction)
{
    mi::base::Handle<const mi::neuraylib::IValue_texture> value(
        resource ? resource->get_interface<const mi::neuraylib::IValue_texture>() : nullptr);
    if (!value || !transaction || (value->get_selector() && *value->get_selector())) return 0;
    mi::base::Handle<const mi::neuraylib::IType_texture> type(value->get_type());
    if (!type || type->get_shape()!=mi::neuraylib::IType_texture::TS_2D) return 0;
    const char *name=value->get_value();
    mi::base::Handle<const mi::neuraylib::ITexture> texture(
        name ? transaction->access<const mi::neuraylib::ITexture>(name) : nullptr);
    mi::base::Handle<const mi::neuraylib::IImage> image(
        texture ? transaction->access<const mi::neuraylib::IImage>(texture->get_image()) : nullptr);
    Texture entry;
    if (!prepareImage(texture.get(), image.get(), m_service, entry, nullptr)) return 0;
    m_textures.push_back(std::move(entry));
    return static_cast<mi::Uint32>(m_textures.size()-1);
}

std::shared_ptr<NativeTextureRuntime> NativeTextureRuntime::Create(
    const mi::neuraylib::ITarget_code *code,
    mi::neuraylib::ITransaction *transaction,std::string *fallbackReason)
{
    auto unsupported=[&](std::string reason)->std::shared_ptr<NativeTextureRuntime> {
        if(fallbackReason) *fallbackReason=std::move(reason);
        return {};
    };
    if (!code || !transaction) return unsupported("missing target or transaction");
    if (code->get_light_profile_count()>1) return unsupported("light-profile resource");
    if (code->get_bsdf_measurement_count()>1) return unsupported("measured-BSDF resource");
    auto result = std::shared_ptr<NativeTextureRuntime>(new NativeTextureRuntime);
    result->m_texcoordSceneData.assign(code->get_string_constant_count(), false);
    for (mi::Size i=1; i<code->get_string_constant_count(); ++i)
        result->m_texcoordSceneData[i]=IsTexcoordPrimvarName(code->get_string_constant(i));
    result->m_textures.resize(code->get_texture_count());
    for (mi::Size i=1; i<code->get_texture_count(); ++i) {
        const auto shape=code->get_texture_shape(i);
        if(shape==mi::neuraylib::ITarget_code::Texture_shape_bsdf_data) {
            mi::Size width=0,height=0,depth=0;const char* pixelType=nullptr;
            const float* data=code->get_texture_df_data(i,width,height,depth,pixelType);
            if(std::getenv("PRISM_MDL_VERBOSE"))
                std::cerr<<"MDL BSDF table "<<i<<" name="
                    <<(code->get_texture(i)?code->get_texture(i):"<null>")
                    <<" kind="<<unsigned(code->get_texture_df_data_kind(i))
                    <<" dimensions="<<width<<'x'<<height<<'x'<<depth
                    <<" pixel="<<(pixelType?pixelType:"<null>")
                    <<" payload="<<(data?"yes":"no")<<'\n';
            std::string tableError;
            result->m_textures[i].dfData=CopyDfData(
                code->get_texture_df_data_kind(i),width,height,depth,pixelType,data,&tableError);
            if(!result->m_textures[i].dfData)
                return unsupported("BSDF data resource "+std::to_string(i)+": "+tableError);
            continue;
        }
        if (shape != mi::neuraylib::ITarget_code::Texture_shape_2d)
            return unsupported("unsupported texture shape " + std::to_string(code->get_texture_shape(i)) +
                " at resource " + std::to_string(i));
        // Selector expressions require channel swizzling during preparation;
        // leave the complete target on the SDK handler until that is present.
        if (const char *selector=code->get_texture_selector(i); selector && *selector)
            return unsupported("texture selector: " + std::string(selector));
        const char *dbName=code->get_texture(i);
        mi::base::Handle<const mi::neuraylib::ITexture> texture(
            dbName ? transaction->access<const mi::neuraylib::ITexture>(dbName) : nullptr);
        if (!texture) return unsupported("missing texture database entry");
        mi::base::Handle<const mi::neuraylib::IImage> image(
            transaction->access<const mi::neuraylib::IImage>(texture->get_image()));
        std::string preparationError;
        if (!prepareImage(texture.get(), image.get(), result->m_service,
                          result->m_textures[i], &preparationError))
            return unsupported(std::move(preparationError));
    }
    return result;
}

mi::neuraylib::Texture_handler_deriv_vtable const &NativeTextureRuntime::Vtable()
{
    static const auto table=[] {
        mi::neuraylib::Texture_handler_deriv_vtable v{};
        v.m_tex_lookup_float4_2d=[](float out[4],Base const *base,unsigned index,
            Deriv2 const *coord,mi::neuraylib::Tex_wrap_mode wu,
            mi::neuraylib::Tex_wrap_mode wv,float const cu[2],float const cv[2],float) {
            out[0]=out[1]=out[2]=out[3]=0.0f;
            auto const *self=Self(base);
            if (!self || index>=self->m_textures.size() || !coord) return;
            auto const &texture=self->m_textures[index];
            const float sourceS=coord->val.x,sourceT=coord->val.y;
            if(!std::isfinite(sourceS) || !std::isfinite(sourceT)) return;
            if(texture.tiled && (sourceS < -2147483648.0 || sourceS >= 2147483648.0 ||
                                 sourceT < -2147483648.0 || sourceT >= 2147483648.0)) return;
            int tileU=texture.tiled ? int(std::floor(sourceS)) : 0;
            int tileV=texture.tiled ? int(std::floor(sourceT)) : 0;
            auto found=texture.tiles.find({tileU,tileV});
            if (found==texture.tiles.end()) return;
            float s=texture.tiled ? sourceS-tileU : sourceS;
            float t=texture.tiled ? sourceT-tileV : sourceT;
            float dsdx=coord->dx.x,dsdy=coord->dy.x;
            float dtdx=coord->dx.y,dtdy=coord->dy.y;
            if (!Remap(s,dsdx,dsdy,wu) || !Remap(t,dtdx,dtdy,wv)) return;
            std::shared_ptr<const prism::TextureResource> resource;
            try { resource=self->croppedResource(found->second,cu,cv); }
            catch(std::exception const& error) {
                if(!self->m_reportedLookupFailure.exchange(true))
                    std::cerr << "MDL texture crop preparation failed: " << error.what() << '\n';
            }
            if(!resource) { out[0]=out[2]=out[3]=1.0f; return; }
            prism::TextureLookup q;
            // MDL canvas coordinates start at the lower-left; OIIO texture
            // coordinates start at the upper-left. Flip derivatives as well.
            q.s=s; q.t=1.0f-t;
            q.dsdx=dsdx; q.dtdx=-dtdx;
            q.dsdy=dsdy; q.dtdy=-dtdy;
            // Wrapping is already applied in the MDL coordinate domain above.
            q.wrapS=Wrap(wu); q.wrapT=Wrap(wv);
            q.filtering=(dsdx==0 && dsdy==0 && dtdx==0 && dtdy==0)
                ? prism::TextureFiltering::Bilinear : prism::TextureFiltering::Filtered;
            if (!self->m_service->lookup(*resource,q,4,out)) {
                out[0]=out[2]=out[3]=1.0f; out[1]=0.0f;
                if (!self->m_reportedLookupFailure.exchange(true))
                    std::cerr << "MDL TextureSystem lookup failed; affected texels are magenta\n";
            } else ExpandChannels(resource->channels(),out);
        };
        v.m_tex_lookup_float3_2d=[](float out[3],Base const *base,unsigned index,
            Deriv2 const *coord,mi::neuraylib::Tex_wrap_mode wu,
            mi::neuraylib::Tex_wrap_mode wv,float const cu[2],float const cv[2],float frame) {
            float tmp[4]{};
            Vtable().m_tex_lookup_float4_2d(tmp,base,index,coord,wu,wv,cu,cv,frame);
            out[0]=tmp[0];out[1]=tmp[1];out[2]=tmp[2];
        };
        v.m_tex_lookup_float4_3d=[](float out[4],Base const *base,unsigned index,
            float const coord[3],mi::neuraylib::Tex_wrap_mode wu,
            mi::neuraylib::Tex_wrap_mode wv,mi::neuraylib::Tex_wrap_mode ww,
            float const cu[2],float const cv[2],float const cw[2],float) {
            out[0]=out[1]=out[2]=out[3]=0.0f;
            auto const *self=Self(base);
            if(!self || !coord || index>=self->m_textures.size())return;
            auto const& table=self->m_textures[index].dfData;
            if(!table)return;
            size_t ox=0,oy=0,oz=0,nx=0,ny=0,nz=0;
            if(!CropAxis(cu,table->width,&ox,&nx) ||
               !CropAxis(cv,table->height,&oy,&ny) ||
               !CropAxis(cw,table->depth,&oz,&nz))return;
            int64_t x0=0,x1=0,y0=0,y1=0,z0=0,z1=0;float fx=0,fy=0,fz=0;
            if(!RemapTexel(coord[0],nx,wu,ox,&x0,&x1,&fx) ||
               !RemapTexel(coord[1],ny,wv,oy,&y0,&y1,&fy) ||
               !RemapTexel(coord[2],nz,ww,oz,&z0,&z1,&fz))return;
            float corners[8][4];
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x0,y0,z0,corners[0]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x1,y0,z0,corners[1]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x0,y1,z0,corners[2]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x1,y1,z0,corners[3]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x0,y0,z1,corners[4]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x1,y0,z1,corners[5]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x0,y1,z1,corners[6]);
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,x1,y1,z1,corners[7]);
            auto lerp=[](float a,float b,float t){return (1.0f-t)*a+t*b;};
            for(int c=0;c<4;++c) {
                const float a=lerp(lerp(corners[0][c],corners[1][c],fx),
                                   lerp(corners[2][c],corners[3][c],fx),fy);
                const float b=lerp(lerp(corners[4][c],corners[5][c],fx),
                                   lerp(corners[6][c],corners[7][c],fx),fy);
                out[c]=lerp(a,b,fz);
            }
        };
        v.m_tex_lookup_float3_3d=[](float out[3],Base const *base,unsigned index,
            float const coord[3],mi::neuraylib::Tex_wrap_mode wu,
            mi::neuraylib::Tex_wrap_mode wv,mi::neuraylib::Tex_wrap_mode ww,
            float const cu[2],float const cv[2],float const cw[2],float frame) {
            float value[4]{};
            Vtable().m_tex_lookup_float4_3d(value,base,index,coord,wu,wv,ww,cu,cv,cw,frame);
            out[0]=value[0];out[1]=value[1];out[2]=value[2];
        };
        v.m_tex_texel_float4_3d=[](float out[4],Base const *base,unsigned index,
            int const coord[3],float) {
            out[0]=out[1]=out[2]=out[3]=0.0f;
            auto const *self=Self(base);
            if(!self || !coord || index>=self->m_textures.size())return;
            auto const& table=self->m_textures[index].dfData;
            if(!table)return;
            LoadDfTexel(table->values.data(),table->width,table->height,table->depth,
                        table->components,coord[0],coord[1],coord[2],out);
        };
        v.m_tex_texel_float4_2d=[](float out[4],Base const *base,unsigned index,
            int const coord[2],int const uvTile[2],float) {
            out[0]=out[1]=out[2]=out[3]=0.0f;
            auto const *self=Self(base);
            if(!self || index>=self->m_textures.size()) return;
            auto found=self->m_textures[index].tiles.find({uvTile[0],uvTile[1]});
            if(found==self->m_textures[index].tiles.end()) return;
            if(coord[0]<0 || coord[1]<0 || coord[0]>=found->second->width() ||
               coord[1]>=found->second->height()) return;
            prism::TextureLookup q;
            q.s=(coord[0]+0.5f)/found->second->width();
            q.t=1.0f-(coord[1]+0.5f)/found->second->height();
            q.nearest=true; q.filtering=prism::TextureFiltering::Bilinear;
            if(self->m_service->lookup(*found->second,q,4,out))
                ExpandChannels(found->second->channels(),out);
        };
        v.m_tex_texture_isvalid=[](Base const *base,unsigned index) {
            auto const *self=Self(base);
            return self && index<self->m_textures.size() &&
                (!self->m_textures[index].tiles.empty() ||
                 bool(self->m_textures[index].dfData));
        };
        v.m_tex_resolution_2d=[](int out[2],Base const *base,unsigned index,
            int const tile[2],float) {
            out[0]=out[1]=0; auto const *self=Self(base);
            if (!self || index>=self->m_textures.size()) return;
            auto found=self->m_textures[index].tiles.find({tile[0],tile[1]});
            if(found==self->m_textures[index].tiles.end()) return;
            out[0]=found->second->width();out[1]=found->second->height();
        };
        v.m_tex_resolution_3d=[](int out[3],Base const *base,unsigned index,float) {
            out[0]=out[1]=out[2]=0;auto const *self=Self(base);
            if(!self || index>=self->m_textures.size())return;
            auto const& table=self->m_textures[index].dfData;if(!table)return;
            out[0]=static_cast<int>(table->width);out[1]=static_cast<int>(table->height);
            out[2]=static_cast<int>(table->depth);
        };
        v.m_tex_frame=[](int out[2],Base const*,unsigned){out[0]=out[1]=0;};
        // The state parameter type differs between MDL SDK releases; the
        // generic lambda converts to either function-pointer signature.
        v.m_adapt_normal=[](float out[3],Base const*,
            auto* /*state*/,float const n[3]) {
            out[0]=n[0];out[1]=n[1];out[2]=n[2]; };
        // Scene data: the texture coordinates under their usual primvar
        // names; other primvars return the defaults. States carry derivatives.
        v.m_scene_data_isvalid=[](Base const* base,
            mi::neuraylib::Shading_state_material*,unsigned id){
            return Self(base)->isTexcoordSceneData(id);};
        v.m_scene_data_lookup_float=[](Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,float d,bool){return d;};
        v.m_scene_data_lookup_float2=[](float out[2],Base const* base,
            mi::neuraylib::Shading_state_material* state,unsigned id,float const d[2],bool){
            if(state && Self(base)->isTexcoordSceneData(id)) {
                auto const* uv=reinterpret_cast<mi::neuraylib::Shading_state_material_with_derivs*>(
                    state)->text_coords;
                out[0]=uv[0].val.x;out[1]=uv[0].val.y;return;
            }
            out[0]=d[0];out[1]=d[1];};
        v.m_scene_data_lookup_float3=[](float out[3],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,float const d[3],bool){
            for(int i=0;i<3;++i)out[i]=d[i];};
        v.m_scene_data_lookup_float4=[](float out[4],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,float const d[4],bool){
            for(int i=0;i<4;++i)out[i]=d[i];};
        v.m_scene_data_lookup_int=[](Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,int d,bool){return d;};
        v.m_scene_data_lookup_int2=[](int out[2],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,int const d[2],bool){
            out[0]=d[0];out[1]=d[1];};
        v.m_scene_data_lookup_int3=[](int out[3],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,int const d[3],bool){
            for(int i=0;i<3;++i)out[i]=d[i];};
        v.m_scene_data_lookup_int4=[](int out[4],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,int const d[4],bool){
            for(int i=0;i<4;++i)out[i]=d[i];};
        v.m_scene_data_lookup_color=[](float out[3],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,float const d[3],bool){
            for(int i=0;i<3;++i)out[i]=d[i];};
        v.m_scene_data_lookup_float4x4=[](float out[16],Base const*,
            mi::neuraylib::Shading_state_material*,unsigned,float const d[16],bool){
            for(int i=0;i<16;++i)out[i]=d[i];};
        v.m_scene_data_lookup_deriv_float=[](
            mi::neuraylib::tct_deriv_float *out,Base const*,
            mi::neuraylib::Shading_state_material_with_derivs*,unsigned,
            mi::neuraylib::tct_deriv_float const *d,bool){*out=*d;};
        v.m_scene_data_lookup_deriv_float2=[](
            mi::neuraylib::tct_deriv_arr_float_2 *out,Base const* base,
            mi::neuraylib::Shading_state_material_with_derivs* state,unsigned id,
            mi::neuraylib::tct_deriv_arr_float_2 const *d,bool){
            if(state && Self(base)->isTexcoordSceneData(id)) {
                auto const& uv=state->text_coords[0];
                out->val[0]=uv.val.x;out->val[1]=uv.val.y;
                out->dx[0]=uv.dx.x;out->dx[1]=uv.dx.y;
                out->dy[0]=uv.dy.x;out->dy[1]=uv.dy.y;
                return;
            }
            *out=*d;};
        v.m_scene_data_lookup_deriv_float3=[](
            mi::neuraylib::tct_deriv_arr_float_3 *out,Base const*,
            mi::neuraylib::Shading_state_material_with_derivs*,unsigned,
            mi::neuraylib::tct_deriv_arr_float_3 const *d,bool){*out=*d;};
        v.m_scene_data_lookup_deriv_float4=[](
            mi::neuraylib::tct_deriv_arr_float_4 *out,Base const*,
            mi::neuraylib::Shading_state_material_with_derivs*,unsigned,
            mi::neuraylib::tct_deriv_arr_float_4 const *d,bool){*out=*d;};
        v.m_scene_data_lookup_deriv_color=[](
            mi::neuraylib::tct_deriv_arr_float_3 *out,Base const*,
            mi::neuraylib::Shading_state_material_with_derivs*,unsigned,
            mi::neuraylib::tct_deriv_arr_float_3 const *d,bool){*out=*d;};
        return v;
    }();
    return table;
}
} // namespace mdl
