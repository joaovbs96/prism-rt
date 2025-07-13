#ifndef COLOR3_H
#define COLOR3_H

#include "float3.h"

class Color3 : public Float3 {
    public:
        Color3() : Float3() {}
        Color3(float x)  : Float3(x, x, x) {}
        Color3(float x, float y, float z)  : Float3(x, y, z) {}

        float r() const { return buffer[0]; }
        float g() const { return buffer[1]; }
        float b() const { return buffer[2]; }
};

#endif