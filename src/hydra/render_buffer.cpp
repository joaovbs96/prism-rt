#include "render_buffer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/renderThread.h"

#include <algorithm>
#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

PrismRenderBuffer::PrismRenderBuffer(SdfPath const &id,
                                     HdRenderThread *renderThread)
    : HdRenderBuffer(id), _renderThread(renderThread) {}
PrismRenderBuffer::~PrismRenderBuffer()
{
    if (_renderThread) _renderThread->StopRender();
    _Deallocate();
}

bool PrismRenderBuffer::Allocate(GfVec3i const &d, HdFormat format, bool ms)
{
    if (_renderThread) _renderThread->StopRender();
    std::lock_guard lock(_mutex);
    if (d[0] < 0 || d[1] < 0 || d[2] != 1 || format == HdFormatInvalid) {
        return false;
    }
    _width = static_cast<unsigned int>(d[0]);
    _height = static_cast<unsigned int>(d[1]);
    _format = format;
    _multiSampled = ms;
    _pixels.assign(static_cast<size_t>(_width) * _height *
                       HdDataSizeOfFormat(_format), 0);
    _workingPixels = _pixels;
    _accumulation.assign(static_cast<size_t>(_width) * _height *
                             HdGetComponentCount(_format), 0.0f);
    _sampleCounts.assign(static_cast<size_t>(_width) * _height, 0);
    _hasPublished = false;
    _converged.store(false);
    return true;
}

void PrismRenderBuffer::_Deallocate()
{
    if (_renderThread) _renderThread->StopRender();
    std::lock_guard lock(_mutex);
    _width = _height = 0;
    _format = HdFormatInvalid;
    _pixels.clear();
    _workingPixels.clear();
    _accumulation.clear();
    _sampleCounts.clear();
    _hasPublished = false;
    _converged.store(false);
}

void *PrismRenderBuffer::Map()
{
    _mutex.lock();
    ++_mappers;
    return _pixels.empty() ? nullptr : _pixels.data();
}

void PrismRenderBuffer::Unmap()
{
    --_mappers;
    _mutex.unlock();
}

void PrismRenderBuffer::Resolve() {}

void PrismRenderBuffer::ResetSamples()
{
    std::lock_guard lock(_mutex);
    std::fill(_accumulation.begin(), _accumulation.end(), 0.0f);
    std::fill(_sampleCounts.begin(), _sampleCounts.end(), 0);
    _converged.store(false);
}

static void _StoreComponent(HdFormat format, uint8_t *dst, size_t c, float v)
{
    switch (HdGetComponentFormat(format)) {
    case HdFormatFloat32: reinterpret_cast<float *>(dst)[c] = v; break;
    case HdFormatFloat16: reinterpret_cast<uint16_t *>(dst)[c] = GfHalf(v).bits(); break;
    case HdFormatUNorm8:
        reinterpret_cast<uint8_t *>(dst)[c] = static_cast<uint8_t>(
            std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); break;
    case HdFormatSNorm8:
        reinterpret_cast<int8_t *>(dst)[c] = static_cast<int8_t>(
            std::clamp(v, -1.0f, 1.0f) * 127.0f); break;
    case HdFormatInt32: reinterpret_cast<int32_t *>(dst)[c] = static_cast<int32_t>(v); break;
    default: break;
    }
}

void PrismRenderBuffer::PublishPreviewFloat(
    unsigned int previewWidth, unsigned int previewHeight,
    std::vector<float> const &values, size_t components)
{
    if (previewWidth == 0 || previewHeight == 0 || components == 0 ||
        values.size() < static_cast<size_t>(previewWidth) * previewHeight *
                            components) return;
    std::lock_guard lock(_mutex);
    if (_width == 0 || _height == 0 || _format == HdFormatInvalid) return;
    const size_t arity = HdGetComponentCount(_format);
    const size_t stride = HdDataSizeOfFormat(_format);
    std::vector<uint8_t> encoded(
        static_cast<size_t>(previewWidth) * previewHeight * stride);
    for (size_t pixel = 0;
         pixel < static_cast<size_t>(previewWidth) * previewHeight; ++pixel) {
        uint8_t *dst = encoded.data() + pixel * stride;
        const float *src = values.data() + pixel * components;
        for (size_t c = 0; c < arity; ++c)
            _StoreComponent(_format, dst, c,
                c < components ? src[c] : (c == 3 ? 1.0f : 0.0f));
    }
    std::vector<uint8_t> expandedRow(static_cast<size_t>(_width) * stride);
    for (unsigned int py = 0; py < previewHeight; ++py) {
        const unsigned int y0 = py * _height / previewHeight;
        const unsigned int y1 = (py + 1) * _height / previewHeight;
        for (unsigned int px = 0; px < previewWidth; ++px) {
            const unsigned int x0 = px * _width / previewWidth;
            const unsigned int x1 = (px + 1) * _width / previewWidth;
            const uint8_t *src = encoded.data() +
                (static_cast<size_t>(py) * previewWidth + px) * stride;
            for (unsigned int x = x0; x < x1; ++x)
                std::memcpy(expandedRow.data() + static_cast<size_t>(x) * stride,
                            src, stride);
        }
        for (unsigned int y = y0; y < y1; ++y)
            std::memcpy(_pixels.data() + static_cast<size_t>(y) * _width * stride,
                        expandedRow.data(), expandedRow.size());
    }
    _hasPublished = true;
}

void PrismRenderBuffer::PublishPreviewInt(
    unsigned int previewWidth, unsigned int previewHeight,
    std::vector<int> const &values, size_t components)
{
    if (previewWidth == 0 || previewHeight == 0 || components == 0 ||
        values.size() < static_cast<size_t>(previewWidth) * previewHeight *
                            components) return;
    std::lock_guard lock(_mutex);
    if (_width == 0 || _height == 0 || _format == HdFormatInvalid) return;
    const size_t arity = HdGetComponentCount(_format);
    const size_t stride = HdDataSizeOfFormat(_format);
    std::vector<uint8_t> encoded(
        static_cast<size_t>(previewWidth) * previewHeight * stride);
    for (size_t pixel = 0;
         pixel < static_cast<size_t>(previewWidth) * previewHeight; ++pixel) {
        uint8_t *dst = encoded.data() + pixel * stride;
        const int *src = values.data() + pixel * components;
        for (size_t c = 0; c < arity; ++c) {
            const int v = c < components ? src[c] : 0;
            if (HdGetComponentFormat(_format) == HdFormatInt32)
                reinterpret_cast<int32_t *>(dst)[c] = v;
            else
                _StoreComponent(_format, dst, c, static_cast<float>(v));
        }
    }
    std::vector<uint8_t> expandedRow(static_cast<size_t>(_width) * stride);
    for (unsigned int py = 0; py < previewHeight; ++py) {
        const unsigned int y0 = py * _height / previewHeight;
        const unsigned int y1 = (py + 1) * _height / previewHeight;
        for (unsigned int px = 0; px < previewWidth; ++px) {
            const unsigned int x0 = px * _width / previewWidth;
            const unsigned int x1 = (px + 1) * _width / previewWidth;
            const uint8_t *src = encoded.data() +
                (static_cast<size_t>(py) * previewWidth + px) * stride;
            for (unsigned int x = x0; x < x1; ++x)
                std::memcpy(expandedRow.data() + static_cast<size_t>(x) * stride,
                            src, stride);
        }
        for (unsigned int y = y0; y < y1; ++y)
            std::memcpy(_pixels.data() + static_cast<size_t>(y) * _width * stride,
                        expandedRow.data(), expandedRow.size());
    }
    _hasPublished = true;
}

void PrismRenderBuffer::_Write(unsigned int x, unsigned int y,
                               const float *value, size_t n, bool accumulate)
{
    if (x >= _width || y >= _height || _format == HdFormatInvalid) return;
    const size_t pixel = static_cast<size_t>(y) * _width + x;
    const size_t arity = HdGetComponentCount(_format);
    uint8_t *dst = _workingPixels.data() +
        pixel * HdDataSizeOfFormat(_format);
    if (accumulate) {
        const uint32_t count = ++_sampleCounts[pixel];
        for (size_t c = 0; c < arity; ++c) {
            float &sum = _accumulation[pixel * arity + c];
            sum += c < n ? value[c] : (c == 3 ? 1.0f : 0.0f);
            _StoreComponent(_format, dst, c, sum / static_cast<float>(count));
        }
    } else {
        for (size_t c = 0; c < arity; ++c) {
            _StoreComponent(_format, dst, c,
                            c < n ? value[c] : (c == 3 ? 1.0f : 0.0f));
        }
    }
}

void PrismRenderBuffer::WriteFloat(unsigned int x, unsigned int y,
                                   const float *value, size_t n, bool accumulate)
{
    std::lock_guard lock(_mutex);
    _Write(x, y, value, n, accumulate);
}

void PrismRenderBuffer::WriteInt(unsigned int x, unsigned int y,
                                 const int *value, size_t n)
{
    std::lock_guard lock(_mutex);
    if (x >= _width || y >= _height) return;
    const size_t arity = HdGetComponentCount(_format);
    uint8_t *dst = _workingPixels.data() +
        (static_cast<size_t>(y) * _width + x) * HdDataSizeOfFormat(_format);
    for (size_t c = 0; c < arity; ++c) {
        const int v = c < n ? value[c] : 0;
        if (HdGetComponentFormat(_format) == HdFormatInt32) {
            reinterpret_cast<int32_t *>(dst)[c] = v;
        } else {
            _StoreComponent(_format, dst, c, static_cast<float>(v));
        }
    }
}

void PrismRenderBuffer::Clear(VtValue const &value)
{
    float f[4] = {0, 0, 0, 0};
    int i[4] = {0, 0, 0, 0};
    size_t n = 1;
    bool ints = false;
    if (value.IsHolding<float>()) f[0] = value.UncheckedGet<float>();
    else if (value.IsHolding<GfVec3f>()) {
        auto const &v = value.UncheckedGet<GfVec3f>(); n = 3;
        std::copy(v.data(), v.data() + 3, f);
    } else if (value.IsHolding<GfVec4f>()) {
        auto const &v = value.UncheckedGet<GfVec4f>(); n = 4;
        std::copy(v.data(), v.data() + 4, f);
    } else if (value.IsHolding<int>()) { i[0] = value.UncheckedGet<int>(); ints = true; }
    std::lock_guard lock(_mutex);
    auto clearWorking = [&] {
        std::fill(_accumulation.begin(), _accumulation.end(), 0.0f);
        std::fill(_sampleCounts.begin(), _sampleCounts.end(), 0);
        for (unsigned int y = 0; y < _height; ++y) for (unsigned int x = 0; x < _width; ++x) {
        if (ints) {
            uint8_t *dst = _workingPixels.data() +
                (static_cast<size_t>(y) * _width + x) * HdDataSizeOfFormat(_format);
            for (size_t c = 0; c < HdGetComponentCount(_format); ++c)
                reinterpret_cast<int32_t *>(dst)[c] = c < n ? i[c] : 0;
        } else _Write(x, y, f, n, false);
        }
    };
    clearWorking();
    _pixels = _workingPixels;
    _hasPublished = true;
}

void PrismRenderBuffer::BeginFrame(VtValue const &value)
{
    float f[4] = {0, 0, 0, 0};
    int i[4] = {0, 0, 0, 0};
    size_t n = 1;
    bool ints = false;
    if (value.IsHolding<float>()) f[0] = value.UncheckedGet<float>();
    else if (value.IsHolding<GfVec3f>()) {
        auto const &v = value.UncheckedGet<GfVec3f>(); n = 3;
        std::copy(v.data(), v.data() + 3, f);
    } else if (value.IsHolding<GfVec4f>()) {
        auto const &v = value.UncheckedGet<GfVec4f>(); n = 4;
        std::copy(v.data(), v.data() + 4, f);
    } else if (value.IsHolding<int>()) {
        i[0] = value.UncheckedGet<int>(); ints = true;
    }

    std::lock_guard lock(_mutex);
    std::fill(_accumulation.begin(), _accumulation.end(), 0.0f);
    std::fill(_sampleCounts.begin(), _sampleCounts.end(), 0);
    // Every supported AOV overwrites every working pixel in the first full
    // pass.  Once a preview/complete frame exists, clearing that hidden image
    // on the UI thread is redundant and expensive at viewport resolutions.
    if (_hasPublished) return;
    for (unsigned int y = 0; y < _height; ++y) {
        for (unsigned int x = 0; x < _width; ++x) {
            if (ints) {
                uint8_t *dst = _workingPixels.data() +
                    (static_cast<size_t>(y) * _width + x) *
                        HdDataSizeOfFormat(_format);
                for (size_t c = 0; c < HdGetComponentCount(_format); ++c)
                    reinterpret_cast<int32_t *>(dst)[c] = c < n ? i[c] : 0;
            } else {
                _Write(x, y, f, n, false);
            }
        }
    }
    if (!_hasPublished) _pixels = _workingPixels;
}

void PrismRenderBuffer::Publish()
{
    std::lock_guard lock(_mutex);
    _pixels = _workingPixels;
    _hasPublished = true;
}

PXR_NAMESPACE_CLOSE_SCOPE
