#pragma once

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/span.h>
#include <OpenImageIO/imageio.h>
using namespace OIIO;

#include <filesystem>
#include <string>

#include "float3.h"

class FrameBuffer{
    public:
        ImageBuf buffer;
        ImageSpec spec;

        FrameBuffer(int width, int height) {
            spec = ImageSpec(width, height, 3, TypeDesc::FLOAT);
            buffer = ImageBuf(spec);
        }

        void setPixel(int column, int row, Color3 color) {
            Color3 clamped_color = color.clamp(0.0f, 1.0f);
            buffer.setpixel(column, row, cspan<float>(clamped_color.buffer));
        }

        bool save(const char* file_path) {
            return buffer.write(file_path);
        }

        bool save(std::string file_path) {
            return save(file_path.c_str());
        }

        bool save(std::filesystem::path file_path) {
            return save(file_path.string().c_str());
        }

        void applyGammaCorrection(float gamma = 2.2f) {
            ImageBufAlgo::pow(buffer, buffer, 1.0f / gamma);
        }
};
