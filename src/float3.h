#ifndef FLOAT3_H
#define FLOAT3_H

#include <cmath>
#include <iostream>
#include <algorithm>

class Float3 {
    public:
        float buffer[3];

        Float3() : buffer{0.0f, 0.0f, 0.0f} {}
        Float3(float x)  : buffer{x, x, x} {}
        Float3(float x, float y, float z)  : buffer{x, y, z} {}
        
        float x() const { return buffer[0]; }
        float y() const { return buffer[1]; }
        float z() const { return buffer[2]; }

        float operator[](int i) const {
            return buffer[i];
        }

        float& operator[](int i) {
            return buffer[i];
        }

        Float3 operator+() const {
            return *this;
        }

        Float3 operator-() const {
            return Float3(-buffer[0], -buffer[1], -buffer[2]);
        }

        //////////////////////////////////////////////////////////
        // operations with equal
        //////////////////////////////////////////////////////////
        Float3& operator+=(const Float3& v) {
            buffer[0] += v.buffer[0];
            buffer[1] += v.buffer[1];
            buffer[2] += v.buffer[2];
            return *this;
        }

        Float3& operator-=(const Float3& v) {
            buffer[0] -= v.buffer[0];
            buffer[1] -= v.buffer[1];
            buffer[2] -= v.buffer[2];
            return *this;
        }

        Float3& operator*=(const Float3& v) {
            buffer[0] *= v.buffer[0];
            buffer[1] *= v.buffer[1];
            buffer[2] *= v.buffer[2];
            return *this;
        }

        Float3& operator*=(const float v) {
            buffer[0] *= v;
            buffer[1] *= v;
            buffer[2] *= v;
            return *this;
        }

        Float3& operator/=(const Float3& v) {
            buffer[0] *= (1.0f / v.buffer[0]);
            buffer[1] *= (1.0f / v.buffer[1]);
            buffer[2] *= (1.0f / v.buffer[2]);
            return *this;
        }

        Float3& operator/=(const float v) {
            float factor = 1.0f / v;
            buffer[0] *= factor;
            buffer[1] *= factor;
            buffer[2] *= factor;
            return *this;
        }

        //////////////////////////////////////////////////////////
        // basic operations
        //////////////////////////////////////////////////////////

        Float3 operator+(const Float3& v) const {
            Float3(x() + v.x(), y() + v.y(), z() + v.z());
        }

        Float3 operator-(const Float3& v) const {
            Float3(x() - v.x(), y() - v.y(), z() - v.z());
        }

        Float3 operator*(const Float3& v) const {
            Float3(x() * v.x(), y() * v.y(), z() * v.z());
        }

        Float3 operator/(const Float3& v) const {
            Float3(x() / v.x(), y() / v.y(), z() / v.z());
        }

        Float3 operator*(const float v) const {
            Float3(x() * v, y() * v, z() * v);
        }

        Float3 operator/(const float v) const {
            float factor = 1.0f / v;
            Float3(x() * factor, y() * factor, z() * factor);
        }

        //////////////////////////////////////////////////////////
        // other methods
        //////////////////////////////////////////////////////////

        float dot(const Float3& v) const {
            return x() * v.x() + y() * v.y()+  z() * v.z();
        }
        
        float length_squared() const {
            // length squared of a vector is the dot product of itself
            return dot(*this);
        }

        float length() const {
            return std::sqrtf(length_squared());
        }

        Float3 cross(const Float3& v) const {
            float _x =  y() * v.z() - z() * v.y();
            float _y = -x() * v.z() + z() * v.x();
            float _z =  x() * v.y() - y() * v.x();
            Float3(_x, _y, _z);
        }

        Float3 sqrt() const {
            return Float3(std::sqrtf(x()), std::sqrtf(y()), std::sqrtf(z()));
        }

        Float3 normalize() const {
            float t = 1.0f / length();
            return *this * t;
        }

        Float3 clamp(float min_value, float max_value) const {
            float _x = std::clamp<float>(x(), min_value, max_value);
            float _y = std::clamp<float>(y(), min_value, max_value);
            float _z = std::clamp<float>(z(), min_value, max_value);
            return Float3(_x, _y, _z);
        }

        float max_component() const {
            return std::fmaxf(std::fmaxf(x(), y()), z());
        }

        float min_component() const {
            return std::fminf(std::fminf(x(), y()), z());
        }

        Float3 max(const Float3& v) const {
            Float3(std::fmaxf(x(), v.x()), std::fmaxf(y(), v.y()), std::fmaxf(z(), v.z()));
        }

        Float3 min(const Float3& v) const {
            Float3(std::fminf(x(), v.x()), std::fminf(y(), v.y()), std::fminf(z(), v.z()));
        }

        bool is_zero() const {
            return (x() == 0.0f) && (y() == 0.0f) && (z() == 0.0f);
        }

        Float3 de_nan(const Float3& c) {
            Float3 temp = c;

            if (buffer[0] != buffer[0]) buffer[0] = 0.0f;
            if (buffer[1] != buffer[1]) buffer[1] = 0.0f;
            if (buffer[2] != buffer[2]) buffer[2] = 0.0f;

            return temp;
        }
};

#endif