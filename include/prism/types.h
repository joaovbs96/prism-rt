#pragma once

// Common types of the public PrismRT API (docs/sdk_design.md, section 3.2).
// Standard library and PrismRT types only.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The renderer's own math types (std-only). They stay in src/ because the
// GPU shaders include them; packaging (migration step 6) installs them with
// the public headers.
#include "../../src/float2.h"
#include "../../src/float3.h"
#include "../../src/float4.h"
#include "../../src/matrix4d.h"

namespace prism {

using ::Float2;
using ::Float3;
using ::Float4;
// Color3 is a Float3 (src/float3.h).

// prism::Matrix4d (src/matrix4d.h) follows USD's GfMatrix4d: row vectors
// (p' = p * M), m[row][column], translation in row 3. Transforms in the API
// are local-to-world matrices in that layout.

// Errors of the API (docs/DECISIONS.md: errors are exceptions).
enum class ErrorCode { InvalidArgument, InvalidHandle, NotFound, Unsupported, DeviceLost, OutOfMemory, Cancelled, Internal };

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string const& message) : std::runtime_error(message), m_code(code) {}
    ErrorCode code() const noexcept { return m_code; }

private:
    ErrorCode m_code;
};

// A typed view of caller memory: `count` elements of `components` scalars of
// `type`. With `owner` set, the scene keeps a reference to it until it is
// replaced or the scene goes; without one (or with a stride other than the
// packed one) the scene copies the elements when they are given. The scene
// reads them at commit() (docs/DECISIONS.md: mesh data is copied at commit).
enum class DataType : uint8_t { Float32, Float64, Int32, UInt32, UInt8 };

inline size_t DataTypeSize(DataType type)
{
    switch (type) {
    case DataType::Float64: return 8;
    case DataType::UInt8: return 1;
    default: return 4;
    }
}

struct BufferView {
    const void* data = nullptr;
    size_t count = 0;             // elements
    uint32_t components = 1;      // scalars per element (3 for points)
    DataType type = DataType::Float32;
    size_t strideBytes = 0;       // 0: packed
    std::shared_ptr<const void> owner;  // optional keep-alive

    bool empty() const { return count == 0; }
    size_t elementBytes() const { return size_t(components) * DataTypeSize(type); }

    // Views of std::vectors. With a shared vector the view keeps it alive.
    template <class T>
    static BufferView Of(std::vector<T> const& values, uint32_t components, DataType type)
    {
        BufferView view;
        view.data = values.data();
        view.count = components ? values.size() * sizeof(T) / (size_t(components) * DataTypeSize(type)) : 0;
        view.components = components;
        view.type = type;
        return view;
    }
    template <class T>
    static BufferView Of(std::shared_ptr<const std::vector<T>> const& values, uint32_t components, DataType type)
    {
        BufferView view = Of(*values, components, type);
        view.owner = values;
        return view;
    }
};

// Time samples (section 5, motion reservation): one sample is not animated.
// Only one instant renders until motion blur exists: the sample nearest to
// time 0.
template <class T>
struct TimeSample {
    double time = 0.0;
    T value{};
};
template <class T>
using Sampled = std::vector<TimeSample<T>>;

template <class T>
Sampled<T> Constant(T value)
{
    return Sampled<T>{TimeSample<T>{0.0, std::move(value)}};
}

} // namespace prism
