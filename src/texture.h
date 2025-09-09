#pragma once

#include <iostream>
#include <filesystem>

#include "float3.h"
#include "float4.h"
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

        virtual T sample(float u, float v, const Float3& p) const = 0;

        virtual T sampleInterp(float u, float v, const Float3& p) const = 0;

        virtual void gammaCorrect(float gamma) {}
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

    T value;
};

// templated getValue functions
template<typename T> T getValue(float* ptr);
template<> float getValue<float>(float* ptr) { return ptr[0]; }
template<> Float3 getValue<Float3>(float* ptr) { return Float3(ptr[0], ptr[1], ptr[2]); }
template<> Float4 getValue<Float4>(float* ptr) { return Float4(ptr[0], ptr[1], ptr[2], ptr[3]); }

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
        }

        ImageTexture(const std::filesystem::path &file_path) {
            channel_n = sizeof(T) / sizeof(float); // This is derived from the template class, can be 1, 3 or 4

            std::cout << "Loading File: " << file_path << std::endl;
            buffer = ImageBuf(file_path.string());
            buffer.read(0, 0, true, TypeDesc::FLOAT);

            width = buffer.spec().width;
            height = buffer.spec().height;
            file_channel_n = buffer.spec().nchannels;

            // adjust channel count
            if (file_channel_n != channel_n) {
                buffer = ImageBufAlgo::channels(buffer, channel_n, {});
            }
        }

        void blur(float kernel_size = 5.0f) {
            ImageBuf K = ImageBufAlgo::make_kernel("gaussian", kernel_size, kernel_size);
            buffer = ImageBufAlgo::convolve(buffer, K);
        }

        void gammaCorrect(float gamma) override {
            const float g_ = 1.0 / gamma;
            ImageBufAlgo::pow(buffer, buffer, {g_, g_, g_});
        }

        void adjustExposure(float exposure) {
            float multiplier = std::pow(2.0f, exposure);
            ImageBufAlgo::mul(buffer, buffer, multiplier);
        }

        T sample(float u, float v, const Float3& p) const override {
            auto i = int(std::fabsf(std::fmodf(u, 1.0)) * (width - 1));
            auto j = int(std::fabsf(std::fmodf(v, 1.0)) * (height - 1));
            float pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            buffer.getpixel(i, j, pixel);
            return getValue<T>(pixel);
        }

        T sampleInterp(float u, float v, const Float3& p) const override {
            auto i = std::fabsf(std::fmodf(u, 1.0)) * (width - 1);
            auto j = std::fabsf(std::fmodf(v, 1.0)) * (height - 1);
            float pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            buffer.interppixel(i, j, pixel);
            return getValue<T>(pixel);
        }

        int width;
        int height;
        int file_channel_n; // original channel count from file
        int channel_n;      // effective channel count in use, due to template
        ImageBuf buffer;
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

        void gammaCorrect(float gamma) override {
            _image->gammaCorrect(gamma);
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