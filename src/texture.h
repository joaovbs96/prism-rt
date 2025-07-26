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
};

template <TextureType T = Float3>
class ValueTexture : public Texture<T> {
    public:
        ValueTexture(const T& value) : value(value) {}

        T sample(float u, float v, const Float3& p) const override {
            return value;
        }

    T value;
};

template <TextureType T = Float3>
class ImageTexture : public Texture<T> {
    public:
        ImageTexture(std::vector<unsigned char>* buffer, int _width, int _height, int file_channel_n) {
            // This is derived from the template class, can be 1, 3 or 4
            width = _width;
            height = _height;
            channel_n = sizeof(T) / sizeof(float);

            // handles type conversion with oiio buffers
            const ImageSpec spec(width, height, file_channel_n, TypeDesc::FLOAT);
            ImageBuf oiio_buffer = ImageBuf(spec, InitializePixels::No);
            oiio_buffer.set_pixels(ROI(), TypeDesc::UINT8, buffer->data());

            // adjust channel count
            if (file_channel_n != channel_n) {
                oiio_buffer = ImageBufAlgo::channels(oiio_buffer, channel_n, {});
            }

            // copy content of oiio buffer to vector buffer
            pixels = std::vector<T>(width * height);
            oiio_buffer.get_pixels(ROI(), TypeDesc::FLOAT, &pixels[0]);
        }

        ImageTexture(const std::filesystem::path &file_path) {
            std::cout << "Loading File: " << file_path << std::endl;
            auto inp = ImageInput::open(file_path.string());
            if (!inp)
                return;
            const ImageSpec& spec = inp->spec();
            width            = spec.width;
            height           = spec.height;
            file_channel_n   = spec.nchannels;
            pixels           = std::vector<T>(width * height);

            // This is derived from the template class, can be 1, 3 or 4
            channel_n = sizeof(T) / sizeof(float);

            inp->read_image(0, 0, 0, channel_n, TypeDesc::FLOAT, &pixels[0]);
            inp->close();
        }

        T sample(float u, float v, const Float3& p) const override {
            auto i = int(std::fmodf(u, 1.0) * (width - 1));
            auto j = int(std::fmodf(v, 1.0) * (height - 1));
            return pixels[j * width + i];
        }

        int width;
        int height;
        int file_channel_n; // original channel count from file
        int channel_n;      // effective channel count in use, due to template
        std::vector<T> pixels;
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

        Color3 sample(float u, float v, const Float3& p) const override {
            return _image->sample(u, v, p) * _value->sample(u, v, p);
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
};