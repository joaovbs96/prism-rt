#pragma once

#include <cmath>
#include <iostream>
#include <algorithm>

class Float2 {
    public:
        float buffer[2];

        Float2() : buffer{0.0f, 0.0f} {}
        Float2(float x)  : buffer{x, x} {}
        Float2(float x, float y)  : buffer{x, y} {}
        
        float x() const { return buffer[0]; }
        float y() const { return buffer[1]; }

        float u() const { return buffer[0]; }
        float v() const { return buffer[1]; }

        float operator[](int i) const {
            return buffer[i];
        }

        float& operator[](int i) {
            return buffer[i];
        }
};
