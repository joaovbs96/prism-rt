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

        Float2 operator+() const {
            return *this;
        }

        Float2 operator-() const {
            return Float2(-buffer[0], -buffer[1]);
        }

        //////////////////////////////////////////////////////////
        // operations with equal
        //////////////////////////////////////////////////////////
        Float2& operator+=(const Float2& v) {
            buffer[0] += v.buffer[0];
            buffer[1] += v.buffer[1];
            return *this;
        }

        Float2& operator-=(const Float2& v) {
            buffer[0] -= v.buffer[0];
            buffer[1] -= v.buffer[1];
            return *this;
        }

        Float2& operator*=(const Float2& v) {
            buffer[0] *= v.buffer[0];
            buffer[1] *= v.buffer[1];
            return *this;
        }

        Float2& operator*=(const float v) {
            buffer[0] *= v;
            buffer[1] *= v;
            return *this;
        }

        Float2& operator/=(const Float2& v) {
            buffer[0] *= (1.0f / v.buffer[0]);
            buffer[1] *= (1.0f / v.buffer[1]);
            return *this;
        }

        Float2& operator/=(const float v) {
            float factor = 1.0f / v;
            buffer[0] *= factor;
            buffer[1] *= factor;
            return *this;
        }

        //////////////////////////////////////////////////////////
        // basic operations
        //////////////////////////////////////////////////////////

        Float2 operator+(const Float2& v) const {
            return Float2(x() + v.x(), y() + v.y());
        }

        Float2 operator-(const Float2& v) const {
            return Float2(x() - v.x(), y() - v.y());
        }

        Float2 operator*(const Float2& v) const {
            return Float2(x() * v.x(), y() * v.y());
        }

        Float2 operator/(const Float2& v) const {
            return Float2(x() / v.x(), y() / v.y());
        }

        //////////////////////////////////////////////////////////
        // other methods
        //////////////////////////////////////////////////////////

        float length_squared() const {
            // length squared of a vector is the dot product of itself
            return x() * x() + y() * y();
        }

        float length() const {
            return std::sqrtf(length_squared());
        }

        Float2 sqrt() const {
            return Float2(std::sqrtf(x()), std::sqrtf(y()));
        }

        Float2 gamma_correct() const {
            float temp_buffer[3] = {x(), y()};
            for (int i = 0; i < 3; i++) {
                if (temp_buffer[i] > 0){
                    temp_buffer[i] = std::sqrtf(temp_buffer[i]);
                } else {
                    temp_buffer[i] = 0;
                }
            }
            return Float2(temp_buffer[0], temp_buffer[1]);
        }

        Float2 normalize() const {
            float t = 1.0f / length();
            return *this * t;
        }

        Float2 clamp(float min_value, float max_value) const {
            float _x = std::clamp<float>(x(), min_value, max_value);
            float _y = std::clamp<float>(y(), min_value, max_value);
            return Float2(_x, _y);
        }

        float max_component() const {
            return std::fmaxf(x(), y());
        }

        float min_component() const {
            return std::fminf(x(), y());
        }

        Float2 max(const Float2& v) const {
            return Float2(std::fmaxf(x(), v.x()), std::fmaxf(y(), v.y()));
        }

        Float2 min(const Float2& v) const {
            return Float2(std::fminf(x(), v.x()), std::fminf(y(), v.y()));
        }

        bool is_zero() const {
            return (x() == 0.0f) && (y() == 0.0f);
        }

        bool is_near_zero() const {
            auto s = 1e-8;
            return (std::fabsf(x()) < s) && (std::fabsf(y()) < s);
        }

        Float2 de_nan(const Float2& c) {
            Float2 temp = c;

            if (buffer[0] != buffer[0]) buffer[0] = 0.0f;
            if (buffer[1] != buffer[1]) buffer[1] = 0.0f;

            return temp;
        }
};

inline std::ostream& operator<<(std::ostream& os, const Float2& v) {
    return os << "<" << v.x() << "," << v.y() << ">";
}

inline Float2 operator*(const float t, const Float2& v) {
  return Float2(v.x() * t, v.y() * t);
}

inline Float2 operator/(const Float2& v, const float t) {
  return Float2(v.x() / t, v.y() / t);
}

inline float dot(const Float2& v1, const Float2& v2) {
    return v1.x() * v2.x() + v1.y() * v2.y();
}

inline float cross(const Float2& v1, const Float2& v2) {
    return v1.x() * v2.y() - v1.y() * v2.x();
}
