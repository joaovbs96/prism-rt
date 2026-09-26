#pragma once

#include <cmath>
#include <iostream>
#include <algorithm>

class Float4 {
    public:
        float buffer[4];

        Float4() : buffer{0.0f, 0.0f, 0.0f, 0.0f} {}
        Float4(float x)  : buffer{x, x, x, x} {}
        Float4(float x, float y, float z, float w)  : buffer{x, y, z, w} {}

        float x() const { return buffer[0]; }
        float y() const { return buffer[1]; }
        float z() const { return buffer[2]; }
        float w() const { return buffer[3]; }

        float r() const { return buffer[0]; }
        float g() const { return buffer[1]; }
        float b() const { return buffer[2]; }
        float a() const { return buffer[3]; }

        float operator[](int i) const {
            return buffer[i];
        }

        float& operator[](int i) {
            return buffer[i];
        }

        Float4 operator+() const {
            return *this;
        }

        Float4 operator-() const {
            return Float4(-buffer[0], -buffer[1], -buffer[2], -buffer[3]);
        }

        //////////////////////////////////////////////////////////
        // operations with equal
        //////////////////////////////////////////////////////////
        Float4& operator+=(const Float4& v) {
            buffer[0] += v.buffer[0];
            buffer[1] += v.buffer[1];
            buffer[2] += v.buffer[2];
            buffer[3] += v.buffer[3];
            return *this;
        }

        Float4& operator-=(const Float4& v) {
            buffer[0] -= v.buffer[0];
            buffer[1] -= v.buffer[1];
            buffer[2] -= v.buffer[2];
            buffer[3] -= v.buffer[3];
            return *this;
        }

        Float4& operator*=(const Float4& v) {
            buffer[0] *= v.buffer[0];
            buffer[1] *= v.buffer[1];
            buffer[2] *= v.buffer[2];
            buffer[3] *= v.buffer[3];
            return *this;
        }

        Float4& operator*=(const float v) {
            buffer[0] *= v;
            buffer[1] *= v;
            buffer[2] *= v;
            buffer[3] *= v;
            return *this;
        }

        Float4& operator/=(const Float4& v) {
            buffer[0] *= (1.0f / v.buffer[0]);
            buffer[1] *= (1.0f / v.buffer[1]);
            buffer[2] *= (1.0f / v.buffer[2]);
            buffer[3] *= (1.0f / v.buffer[3]);
            return *this;
        }

        Float4& operator/=(const float v) {
            float factor = 1.0f / v;
            buffer[0] *= factor;
            buffer[1] *= factor;
            buffer[2] *= factor;
            buffer[3] *= factor;
            return *this;
        }

        //////////////////////////////////////////////////////////
        // basic operations
        //////////////////////////////////////////////////////////

        Float4 operator+(const Float4& v) const {
            return Float4(x() + v.x(), y() + v.y(), z() + v.z(), w() + v.w());
        }

        Float4 operator-(const Float4& v) const {
            return Float4(x() - v.x(), y() - v.y(), z() - v.z(), w() - v.w());
        }

        Float4 operator*(const Float4& v) const {
            return Float4(x() * v.x(), y() * v.y(), z() * v.z(), w() - v.w());
        }

        Float4 operator/(const Float4& v) const {
            return Float4(x() / v.x(), y() / v.y(), z() / v.z(), w() - v.w());
        }

        //////////////////////////////////////////////////////////
        // other methods
        //////////////////////////////////////////////////////////

        float length_squared() const {
            // length squared of a vector is the dot product of itself
            return x() * x() + y() * y() + z() * z() + w() + w();
        }

        float length() const {
            return std::sqrtf(length_squared());
        }

        Float4 sqrt() const {
            return Float4(std::sqrtf(x()), std::sqrtf(y()), std::sqrtf(z()), std::sqrtf(w()));
        }

        Float4 gamma_correct() const {
            float temp_buffer[4] = {x(), y(), z()};
            for (int i = 0; i < 4; i++) {
                if (temp_buffer[i] > 0){
                    temp_buffer[i] = std::sqrtf(temp_buffer[i]);
                } else {
                    temp_buffer[i] = 0;
                }
            }
            return Float4(temp_buffer[0], temp_buffer[1], temp_buffer[2], temp_buffer[3]);
        }

        Float4 normalize() const {
            float t = 1.0f / length();
            return *this * t;
        }

        Float4 clamp(float min_value, float max_value) const {
            float _x = std::clamp<float>(x(), min_value, max_value);
            float _y = std::clamp<float>(y(), min_value, max_value);
            float _z = std::clamp<float>(z(), min_value, max_value);
            float _w = std::clamp<float>(w(), min_value, max_value);
            return Float4(_x, _y, _z, _w);
        }

        float max_component() const {
            return std::fmaxf(std::fmaxf(std::fmaxf(x(), y()), z()), w());
        }

        float min_component() const {
            return std::fmaxf(std::fminf(std::fminf(x(), y()), z()), w());
        }

        Float4 max(const Float4& v) const {
            return Float4(
                std::fmaxf(x(), v.x()),
                std::fmaxf(y(), v.y()),
                std::fmaxf(z(), v.z()),
                std::fmaxf(w(), v.w())
            );
        }

        Float4 min(const Float4& v) const {
            return Float4(
                std::fminf(x(), v.x()),
                std::fminf(y(), v.y()),
                std::fminf(z(), v.z()),
                std::fminf(w(), v.w())
            );
        }

        bool is_zero() const {
            return (x() == 0.0f) && (y() == 0.0f) && (z() == 0.0f) && (w() == 0.0f);
        }

        bool is_near_zero() const {
            auto s = 1e-8;
            return (std::fabsf(x()) < s) && (std::fabsf(y()) < s) && (std::fabsf(z()) < s) && (std::fabsf(w()) < s);
        }

        Float4 de_nan(const Float4& c) {
            Float4 temp = c;

            if (buffer[0] != buffer[0]) buffer[0] = 0.0f;
            if (buffer[1] != buffer[1]) buffer[1] = 0.0f;
            if (buffer[2] != buffer[2]) buffer[2] = 0.0f;
            if (buffer[3] != buffer[3]) buffer[3] = 0.0f;

            return temp;
        }
};

inline std::ostream& operator<<(std::ostream& os, const Float4& v) {
    return os << "<" << v.x() << "," << v.y() << "," << v.z() << "," << v.w() << ">";
}

inline Float4 operator*(const float t, const Float4& v) {
  return Float4(v.x() * t, v.y() * t, v.z() * t, v.w() * t);
}

inline Float4 operator/(const Float4& v, const float t) {
  return Float4(v.x() / t, v.y() / t, v.z() / t, v.w() / t);
}

inline float dot(const Float4& v1, const Float4& v2) {
    return v1.x() * v2.x() + v1.y() * v2.y() + v1.z() * v2.z() + v1.w() * v2.w();
}

inline Float4 cross(const Float4& v1, const Float4& v2) {
    // TODO: is this correct?
    float _x =  v1.w() * v2.x() + v1.x() * v2.w() + v1.y() * v2.z() - v1.z() * v2.y();  // x
    float _y =  v1.w() * v2.y() - v1.x() * v2.z() + v1.y() * v2.w() + v1.z() * v2.x();  // y
    float _z =  v1.w() * v2.z() + v1.x() * v2.y() - v1.y() * v2.x() + v1.z() * v2.w();  // z
    float _w =  v1.w() * v2.w() - v1.x() * v2.x() - v1.y() * v2.y() - v1.z() * v2.z();  // w
    return Float4(_x, _y, _z, _w);
}

// Class alias
using Color4 = Float4;
