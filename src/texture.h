#pragma once

#include <iostream>
#include <filesystem>

#include "float3.h"
#include "float4.h"
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/span.h>
#include <OpenImageIO/imageio.h>
using namespace OIIO;

class Texture {
    public:
        Texture() {}

        virtual Color3 sample(float u, float v, const Float3& p) const = 0;
};

class ColorTexture : public Texture {
    public:
        ColorTexture(Color3 color) : color(color) {}

        Color3 sample(float u, float v, const Float3& p) const override {
            return color;
        }

    Color3 color;
};

template<typename T>
concept TextureType = std::same_as<T, float> || std::same_as<T, Float3> || std::same_as<T, Float4>;

template <TextureType T = Float3>
class ImageTexture : public Texture {
    public:
        ImageTexture(const std::filesystem::path &file_path) {
            std::cout << "Loading File: " << file_path << std::endl;
            auto inp = ImageInput::open(file_path.string());
            if (!inp)
                return;
            const ImageSpec& spec = inp->spec();
            width            = spec.width;
            height           = spec.height;
            input_channel_n  = spec.nchannels;
            pixels           = std::vector<T>(width * height);

            // This is derived from the template class, can be 1, 3 or 4
            forced_channel_n = sizeof(T) / sizeof(float);

            inp->read_image(0, 0, 0, forced_channel_n, TypeDesc::FLOAT, &pixels[0]);
            inp->close();
        }

        T sample(float u, float v, const Float3& p) const override {
            auto i = int(std::fmodf(u, 1.0) * (width - 1));
            auto j = int(std::fmodf(v, 1.0) * (height - 1));
            return pixels[j * width + i];
        }

        int width;
        int height;
        int input_channel_n;
        int forced_channel_n;
        std::vector<T> pixels;
};