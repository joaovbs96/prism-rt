#include <iostream>

#include "frame_buffer.h"
#include "color3.h"

int main()
{
    int width = 256;
    int height = 256;

    FrameBuffer frame(width, height);

    for (int row = 0; row < height; row++) {
        for (int column = 0; column < width; column++) {
            int base_index = (row * width + column) * 3;

            auto r = float(row) / (width - 1);
            auto g = float(column) / (height - 1);
            auto b = float(1.0);
            Color3 color(r, g, b);

            frame.setPixel(row, column, color);
        }
    }

    frame.save("output.jpg");

    return 0;
}