#pragma once

#include <iostream>
#include <cmath>
#include <cstring>

#include "float3.h"
#include "float4.h"
#include "math.h"

class TransformMatrix {
    public:
        float* matrix;

        TransformMatrix() {
            matrix = new float[16];
            identity();
        }

        ~TransformMatrix() {
            delete[] matrix;
        }

        // Get the raw float pointer
        float* getMatrix() { return matrix; }
        const float* getMatrix() const { return matrix; }

        // Create identity matrix
        void identity() {
            memset(matrix, 0, 16 * sizeof(float));
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        }

        bool isIdentity() {
            TransformMatrix* temp = new TransformMatrix();
            for (int i = 0; i < 16; i ++){
                if (matrix[i] != temp->matrix[i])
                    return false;
            }
            return true;
        }

        // Translation matrix
        void translate(float x, float y, float z) {
            TransformMatrix* temp = new TransformMatrix();
            temp->matrix[3] = x;  // m[3][0]
            temp->matrix[7] = y;  // m[3][1]
            temp->matrix[11] = z;  // m[3][2]
            multiply(*temp);
            delete temp;
        }

        // Rotation around X-axis
        void rotateX(float degrees) {
            TransformMatrix* temp = new TransformMatrix();
            float radians = deg2rad(degrees);
            float c = cos(radians);
            float s = sin(radians);
            temp->matrix[5] = c;   // m[1][1]
            temp->matrix[6] = -s;  // m[1][2]
            temp->matrix[9] = s;   // m[2][1]
            temp->matrix[10] = c;  // m[2][2]
            multiply(*temp);
            delete temp;
        }

        // Rotation around Y-axis
        void rotateY(float degrees) {
            TransformMatrix* temp = new TransformMatrix();
            float radians = deg2rad(degrees);
            float c = cos(radians);
            float s = sin(radians);
            temp->matrix[0] = c;   // m[0][0]
            temp->matrix[2] = s;   // m[0][2]
            temp->matrix[8] = -s;  // m[2][0]
            temp->matrix[10] = c;  // m[2][2]
            multiply(*temp);
            delete temp;
        }

        // Rotation around Z-axis
        void rotateZ(float degrees) {
            TransformMatrix* temp = new TransformMatrix();
            float radians = deg2rad(degrees);
            float c = cos(radians);
            float s = sin(radians);
            temp->matrix[0] = c;   // m[0][0]
            temp->matrix[1] = -s;  // m[0][1]
            temp->matrix[4] = s;   // m[1][0]
            temp->matrix[5] = c;   // m[1][1]
            multiply(*temp);
            delete temp;
        }

        void rotate(float degrees_x, float degrees_y, float degrees_z) {
            rotateZ(degrees_z);
            rotateY(degrees_y);
            rotateX(degrees_x);
        }

        // Scale matrix
        void scale(float x, float y, float z) {
            TransformMatrix* temp = new TransformMatrix();
            temp->matrix[0] = x;   // m[0][0]
            temp->matrix[5] = y;   // m[1][1]
            temp->matrix[10] = z;  // m[2][2]
            multiply(*temp);
            delete temp;
        }

        // Matrix multiplication
        void multiply(const TransformMatrix& other) {
            TransformMatrix result;
            float* r = result.matrix;
            float* a = matrix;
            const float* b = other.matrix;

            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    r[i * 4 + j] = 0;
                    for (int k = 0; k < 4; k++) {
                        r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
                    }
                }
            }
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    a[i * 4 + j] = r[i * 4 + j];
                }
            }
        }

        Float3 transformPoint(const Float3& p) {
            Float4 temp = Float4(p.x(), p.y(), p.z(), 1.0f);
            Float4 result = transformPoint(temp);
            return Float3(result.x(), result.y(), result.z());
        }

        Float4 transformPoint(const Float4& p) {
            float x_ = p.x() * matrix[0]  + p.y() * matrix[1]  + p.z() * matrix[2]  + p.w() * matrix[3];
            float y_ = p.x() * matrix[4]  + p.y() * matrix[5]  + p.z() * matrix[6]  + p.w() * matrix[7];
            float z_ = p.x() * matrix[8]  + p.y() * matrix[9]  + p.z() * matrix[10] + p.w() * matrix[11];
            float w_ = p.x() * matrix[12] + p.y() * matrix[13] + p.z() * matrix[14] + p.w() * matrix[15];
            return Float4(x_, y_, z_, w_);
        }

        // Print matrix for debugging
        void print() const {
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    std::cout << matrix[i * 4 + j] << "\t";
                }
                std::cout << std::endl;
            }
        }
};