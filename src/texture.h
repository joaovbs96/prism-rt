#pragma once

#include <iostream>
#include <filesystem>

#include "float3.h"
#include "float4.h"
#include "texture_service.h"
#include "color/color_management.h"
#include "ray.h"
#include <memory>
#include <stdexcept>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/span.h>
#include <OpenImageIO/imageio.h>
using namespace OIIO;

template<typename T>
concept TextureType = std::same_as<T, float> || std::same_as<T, Float3> || std::same_as<T, Float4>;

template <TextureType T = Float3>
class Texture {
    public:
        Texture() {}
        virtual ~Texture() = default;

        virtual T sample(float u, float v, const Float3& p) const = 0;

        virtual T sampleInterp(float u, float v, const Float3& p) const = 0;

        virtual T sample(float u, float v, const Float3& p,
                         const prism::TextureFootprint& footprint) const {
            return sample(u, v, p);
        }
        virtual T sampleInterp(float u, float v, const Float3& p,
                               const prism::TextureFootprint& footprint) const {
            return sampleInterp(u, v, p);
        }
        virtual void gammaCorrect(float gamma) {}
        // Colour textures: their values are in `colorSpace` (an OCIO or
        // MaterialX name); convert them into the working space. Call at most
        // once, and not on data (normals, roughness).
        virtual void convertToWorkingSpace(std::string const& colorSpace) {}
};

template <TextureType T = Float3>
class ValueTexture : public Texture<T> {
    public:
        ValueTexture(const T& value) : value(value) {}

        T sample(float u, float v, const Float3& p) const override {
            return value;
        }

        T sampleInterp(float u, float v, const Float3& p) const override {
            return value;
        }

        void convertToWorkingSpace(std::string const& colorSpace) override {
            if constexpr (!std::same_as<T, float>) {
                auto color = prism::color::Active();
                if (!color->NeedsConversion(colorSpace)) return;
                float rgb[3] = {value[0], value[1], value[2]};
                color->ToWorkingSpace(colorSpace, rgb, 1, 3);
                for (int c = 0; c < 3; ++c) value[c] = rgb[c];
            }
        }

    T value;
};

// templated getValue functions
template<typename T> T getValue(float* ptr);
template<> inline float getValue<float>(float* ptr) { return ptr[0]; }
template<> inline Float3 getValue<Float3>(float* ptr) { return Float3(ptr[0], ptr[1], ptr[2]); }
template<> inline Float4 getValue<Float4>(float* ptr) { return Float4(ptr[0], ptr[1], ptr[2], ptr[3]); }

template <TextureType T = Float3>
class ImageTexture : public Texture<T> {
    public:
        // TODO: make the pixel buffer be an ImageBuf and operate/sample from that with getPixel
        ImageTexture(std::vector<unsigned char>* disk_buffer, int _width, int _height, int file_channel_n) {
            width = _width;
            height = _height;
            channel_n = sizeof(T) / sizeof(float); // This is derived from the template class, can be 1, 3 or 4

            // handles type conversion with oiio buffers
            const ImageSpec spec(width, height, file_channel_n, TypeDesc::FLOAT);
            buffer = ImageBuf(spec, InitializePixels::No);
            buffer.set_pixels(ROI(), TypeDesc::UINT8, disk_buffer->data());

            // adjust channel count
            if (file_channel_n != channel_n) {
                buffer = ImageBufAlgo::channels(buffer, channel_n, {});
            }
            publishEdits();
        }

        ImageTexture(const std::filesystem::path &file_path)
            : service(prism::TextureService::Acquire()), source(file_path) {
            channel_n = sizeof(T) / sizeof(float);
            prepareOptions.channelCount = channel_n;
            publishResource();
        }

        // A colour image whose texels are in `colorSpace`, prepared in the
        // working space (TexturePrepareOptions::workingSpace).
        ImageTexture(const std::filesystem::path &file_path, std::string const &colorSpace)
            : service(prism::TextureService::Acquire()), source(file_path) {
            channel_n = sizeof(T) / sizeof(float);
            prepareOptions.channelCount = channel_n;
            setColorSpace(colorSpace);
            publishResource();
        }

        void convertToWorkingSpace(std::string const& colorSpace) override {
            if (!resource) {
                // Mutable buffers are published by publishEdits.
                std::vector<float> pixels(size_t(width) * height * channel_n);
                buffer.get_pixels(ROI(0, width, 0, height, 0, 1, 0, channel_n), TypeDesc::FLOAT, pixels.data());
                prism::color::Active()->ToWorkingSpace(colorSpace, pixels.data(), size_t(width) * height, channel_n);
                buffer.set_pixels(ROI(0, width, 0, height, 0, 1, 0, channel_n), TypeDesc::FLOAT, pixels.data());
                return;
            }
            if (setColorSpace(colorSpace)) publishResource();
        }

        // Explicit boundary for callers editing the public ImageBuf (fixtures
        // and procedural images). Published renderer resources remain immutable.
        void publishEdits() {
            if (!service) service = prism::TextureService::Acquire();
            resource = service->prepare(buffer);
            source = resource->preparedPath();
            prepareOptions = {};
            useResource();
        }

        void blur(float kernel_size = 5.0f) {
            ImageBuf K = ImageBufAlgo::make_kernel("gaussian", kernel_size, kernel_size);
            buffer = ImageBufAlgo::convolve(buffer, K);
            publishEdits();
        }

        void gammaCorrect(float gamma) override {
            if (!(gamma > 0) || !std::isfinite(gamma))
                throw std::runtime_error("Invalid image gamma");
            if (!resource) { ImageBufAlgo::pow(buffer, buffer, {1/gamma,1/gamma,1/gamma}); return; }
            prepareOptions.gamma /= gamma;
            prepareOptions.exposure /= gamma;
            publishResource();
        }

        void adjustExposure(float exposure) {
            if (!std::isfinite(exposure)) throw std::runtime_error("Invalid image exposure");
            if (!resource) { ImageBufAlgo::mul(buffer, buffer, std::pow(2.0f, exposure)); return; }
            prepareOptions.exposure += exposure;
            publishResource();
        }

        T lookup(prism::TextureLookup options) const {
            float pixel[4]{};
            if (resource) {
                if (!service->lookup(*resource, options, channel_n, pixel))
                    throw std::runtime_error("Texture lookup failed: " + source.string());
            } else {
                // Procedural/embedded mutable buffers are published explicitly.
                buffer.interppixel(options.s * width, options.t * height, pixel);
            }
            return getValue<T>(pixel);
        }

        bool readRow(int y, float* pixels, int channels) const {
            if (resource) return service->readPixels(*resource, 0, y, width, y+1, 0, channels, pixels);
            return buffer.get_pixels(ROI(0,width,y,y+1,0,1,0,channels), TypeDesc::FLOAT, pixels);
        }

        T sample(float u, float v, const Float3& p,
                 const prism::TextureFootprint& footprint) const override {
            if (footprint.filtering == prism::TextureFiltering::Bilinear) return sample(u,v,p);
            return sampleFiltered(u,v,footprint);
        }
        T sampleInterp(float u, float v, const Float3& p,
                       const prism::TextureFootprint& footprint) const override {
            if (footprint.filtering == prism::TextureFiltering::Bilinear) return sampleInterp(u,v,p);
            return sampleFiltered(u,v,footprint);
        }

        T sample(float u, float v, const Float3& p) const override {
            auto i = int(std::fabsf(std::fmodf(u, 1.0)) * (width - 1));
            auto j = int(std::fabsf(std::fmodf(v, 1.0)) * (height - 1));
            float pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (resource) {
                prism::TextureLookup options{};
                options.s = (i + .5f) / width; options.t = (j + .5f) / height;
                options.filtering = prism::TextureFiltering::Bilinear;
                options.nearest = true;
                return lookup(options);
            }
            buffer.getpixel(i, j, pixel);
            return getValue<T>(pixel);
        }

        T sampleInterp(float u, float v, const Float3& p) const override {
            auto i = std::fabsf(std::fmodf(u, 1.0)) * (width - 1);
            auto j = std::fabsf(std::fmodf(v, 1.0)) * (height - 1);
            float pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (resource) {
                prism::TextureLookup options{};
                options.s = i / width; options.t = j / height;
                options.filtering = prism::TextureFiltering::Bilinear;
                options.wrapS = options.wrapT = prism::TextureWrap::Black;
                return lookup(options);
            }
            buffer.interppixel(i, j, pixel);
            return getValue<T>(pixel);
        }

        int width;
        int height;
        int file_channel_n; // original channel count from file
        int channel_n;      // effective channel count in use, due to template
        ImageBuf buffer;
        std::shared_ptr<prism::TextureService> service;
        std::shared_ptr<const prism::TextureResource> resource;
    private:
        std::filesystem::path source;
        prism::TexturePrepareOptions prepareOptions;
        void useResource() {
            width = resource->width(); height = resource->height();
            file_channel_n = resource->channels();
            // Lazy access for preparation/legacy fixtures; no duplicate pixel array.
            buffer = ImageBuf(resource->preparedPath().string());
        }
        void publishResource() {
            resource = service->prepare(source, prepareOptions);
            useResource();
        }
        // Whether texels in colorSpace need converting; sets the options.
        bool setColorSpace(std::string const &colorSpace) {
            if constexpr (std::same_as<T, float>) return false;
            auto color = prism::color::Active();
            if (!color->NeedsConversion(colorSpace)) {
                if (!colorSpace.empty() && color->Resolve(colorSpace).empty())
                    throw std::runtime_error("Unknown texture colour space '" + colorSpace + "': " + source.string());
                return false;
            }
            prepareOptions.colorSpace = colorSpace;
            prepareOptions.workingSpace = color->identity();
            return true;
        }
        T sampleFiltered(float u, float v, const prism::TextureFootprint& footprint) const {
            prism::TextureLookup options{};
            options.s = u; options.t = v;
            options.dsdx = footprint.dudx; options.dtdx = footprint.dvdx;
            options.dsdy = footprint.dudy; options.dtdy = footprint.dvdy;
            options.wrapS = options.wrapT = prism::TextureWrap::Periodic;
            options.filtering = footprint.filtering;
            return lookup(options);
        }
};

template <TextureType T = Float3>
class ImageMixTexture : public Texture<T> {
    public:
        ImageMixTexture(const std::filesystem::path &file_path, const T& v = T(1.0f)) {
            _image = new ImageTexture<T>(file_path);
            _value = new ValueTexture<T>(v);
        }

        ImageMixTexture(std::vector<unsigned char>* buffer, int _width, int _height, int file_channel_n, const T& v = T(1.0f)) {
            _image = new ImageTexture<T>(buffer, _width, _height, file_channel_n);
            _value = new ValueTexture<T>(v);
        }

        T sample(float u, float v, const Float3& p) const override {
            return _image->sample(u, v, p) * _value->sample(u, v, p);
        }

        T sampleInterp(float u, float v, const Float3& p) const override {
            return _image->sampleInterp(u, v, p) * _value->sampleInterp(u, v, p);
        }

        T sample(float u, float v, const Float3& p, const prism::TextureFootprint& fp) const override {
            return _image->sample(u,v,p,fp) * _value->sample(u,v,p);
        }
        T sampleInterp(float u, float v, const Float3& p, const prism::TextureFootprint& fp) const override {
            return _image->sampleInterp(u,v,p,fp) * _value->sampleInterp(u,v,p);
        }
        void gammaCorrect(float gamma) override {
            _image->gammaCorrect(gamma);
        }
        // Image and factor convert separately (exact when either is grey);
        // the GPU devices multiply the converted pair as well.
        void convertToWorkingSpace(std::string const& colorSpace) override {
            _image->convertToWorkingSpace(colorSpace);
            _value->convertToWorkingSpace(colorSpace);
        }

    ImageTexture<T>* _image;
    ValueTexture<T>* _value;
};

class ColorTexture : public ValueTexture<Float3> {
    public:
        ColorTexture(Color3 color) : ValueTexture<Float3>(color) {}

        Color3 sample(float u, float v, const Float3& p) const override {
            return value;
        }

        Color3 sampleInterp(float u, float v, const Float3& p) const override {
            return value;
        }

        void gammaCorrect(float gamma) override {
            value = value.gamma_correct(gamma);
        }
};
