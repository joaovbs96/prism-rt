#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/span.h>
#include <OpenImageIO/imageio.h>
using namespace OIIO;

#include "color3.h"

class FrameBuffer{
    public:
        ImageBuf buffer;
        ImageSpec spec;

        FrameBuffer(int width, int height) {
            spec = ImageSpec(width, height, 3, TypeDesc::FLOAT);
            buffer = ImageBuf(spec);
        }

        void setPixel(int row, int column, Color3 color) {
            buffer.setpixel(row, column, cspan<float>(color.buffer));
        }

        bool save(const char* file_path) {
            return buffer.write(file_path);
        }
};