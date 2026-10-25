#include "render_buffer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/renderThread.h"

#include <algorithm>
#include <cmath>
#include <tbb/parallel_for.h>
#include <cstring>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

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
    switch (HdGetComponentFormat(format)) {
    case HdFormatFloat32:
    case HdFormatFloat16:
    case HdFormatUNorm8:
    case HdFormatSNorm8:
    case HdFormatInt16:
    case HdFormatUInt16:
    case HdFormatInt32:
        break;
    default:
        TF_WARN("PrismRT render buffer does not support format %d",
                static_cast<int>(format));
        return false;
    }
    _width = static_cast<unsigned int>(d[0]);
    _height = static_cast<unsigned int>(d[1]);
    _format = format;
    _multiSampled = ms;
    _pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(_format), 0);
    _frame.reset();
    _ClearRefinement();
    _presentationSource.reset();
    _presentationPixels.reset();
    _workingPixels = *_pixels;
    _accumulation.assign(static_cast<size_t>(_width) * _height *
                             HdGetComponentCount(_format), 0.0f);
    _sampleCounts.assign(static_cast<size_t>(_width) * _height, 0);
    _hasPublished = false;
    _initialCleared = false;
    _converged.store(false);
    _allocationRevision.fetch_add(1);
    return true;
}

void PrismRenderBuffer::_Deallocate()
{
    if (_renderThread) _renderThread->StopRender();
    std::lock_guard lock(_mutex);
    _width = _height = 0;
    _allocationRevision.fetch_add(1);
    _format = HdFormatInvalid;
    _pixels.reset();
    _frame.reset();
    _ClearRefinement();
    _presentationSource.reset();
    _presentationPixels.reset();
    _workingPixels.clear();
    _accumulation.clear();
    _sampleCounts.clear();
    _hasPublished = false;
    _initialCleared = false;
    _converged.store(false);
}

void *PrismRenderBuffer::Map()
{
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(_mutex);
    _mapWaitNanoseconds.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count()));
    if (_mappers.fetch_add(1) == 0) {
        _mappedPixels = _pixels;
        _mappedFrame = _frame;
        _mappedWidth=_width; _mappedHeight=_height; _mappedFormat=_format;
    }
    return !_mappedPixels || _mappedPixels->empty() ? nullptr : _mappedPixels->data();
}

void PrismRenderBuffer::Unmap()
{
    std::lock_guard lock(_mutex);
    if (!TF_VERIFY(_mappers.load() > 0)) return;
    if (_mappers.fetch_sub(1) == 1) {
        _mappedPixels.reset(); _mappedFrame.reset();
    }
}

PrismRenderBuffer::ReadView PrismRenderBuffer::Capture() const
{
    std::lock_guard lock(_mutex);
    if(_mappers.load()) return {_mappedPixels,_mappedFrame,_mappedWidth,_mappedHeight,_mappedFormat};
    return {_pixels,_frame,_width,_height,_format};
}

void PrismRenderBuffer::SetPresentationClearColor(VtValue const &value)
{
    std::lock_guard lock(_mutex);
    const bool hasColor = value.IsHolding<GfVec4f>();
    const GfVec4f color = hasColor ? value.UncheckedGet<GfVec4f>() : GfVec4f(0.0f);
    if (hasColor != _hasPresentationClearColor || color != _presentationClearColor) {
        _presentationSource.reset();
        _presentationPixels.reset();
    }
    _hasPresentationClearColor = hasColor;
    _presentationClearColor = color;
}

PrismRenderBuffer::ReadView PrismRenderBuffer::CapturePublished() const
{
    std::lock_guard lock(_mutex);
    return {_pixels,_frame,_width,_height,_format};
}

void PrismRenderBuffer::SetPresentationPixels(
    std::shared_ptr<const std::vector<uint8_t>> pixels, Frame frame)
{
    std::lock_guard lock(_mutex);
    if (!pixels || !frame || frame != _frame || !_pixels || pixels->size()!=_pixels->size()) return;
    _ClearRefinement();
    _refinementPixels=std::move(pixels); _refinementFrame=std::move(frame);
}

void PrismRenderBuffer::_ClearRefinement()
{
    _refinementPixels.reset(); _refinementFrame.reset();
    _presentationSnapshot.reset();
    _materializedSnapshot.reset(); _materializedPixels.reset();
    _presentationSource.reset(); _presentationPixels.reset();
}

void PrismRenderBuffer::SetPresentationSnapshot(
    std::shared_ptr<const PresentationSnapshot> snapshot)
{
    if (!snapshot) return;
    std::lock_guard lock(_mutex);
    auto const &base = snapshot->base;
    if (base.pixels != _pixels || !base.frame || base.frame != _frame ||
        base.width != _width || base.height != _height ||
        base.format != HdFormatFloat32Vec4 || _format != base.format) return;
    for (auto const &tile : snapshot->tiles) {
        if (!tile) continue;
        if (!tile->width || !tile->height || tile->width > 32 || tile->height > 32 ||
            tile->x >= _width || tile->y >= _height ||
            tile->width > _width-tile->x || tile->height > _height-tile->y ||
            tile->rgba.size() != size_t(tile->width)*tile->height*4) return;
    }
    if (_presentationSnapshot && _presentationSnapshot->base.pixels == base.pixels &&
        (snapshot->pass < _presentationSnapshot->pass ||
         (snapshot->pass == _presentationSnapshot->pass &&
          snapshot->revision <= _presentationSnapshot->revision))) return;
    _ClearRefinement();
    _presentationSnapshot = std::move(snapshot);
}

PrismRenderBuffer::PresentationState PrismRenderBuffer::CapturePresentationState() const
{
    std::lock_guard lock(_mutex);
    PresentationState result;
    result.raw = Capture();
    result.background = _presentationClearColor;
    result.hasBackground = _hasPresentationClearColor &&
        result.raw.format == HdFormatFloat32Vec4;
    if (_presentationSnapshot &&
        _presentationSnapshot->base.pixels == result.raw.pixels &&
        _presentationSnapshot->base.frame == result.raw.frame &&
        result.raw.width == _width && result.raw.height == _height &&
        result.raw.format == _format) result.snapshot = _presentationSnapshot;
    if (_refinementPixels && result.raw.frame == _refinementFrame &&
        result.raw.width == _width && result.raw.height == _height &&
        result.raw.format == _format) result.legacyPixels = _refinementPixels;
    return result;
}

void PrismRenderBuffer::ClearPresentationPixels()
{
    std::lock_guard lock(_mutex);
    _ClearRefinement();
}

PrismRenderBuffer::ReadView PrismRenderBuffer::CapturePresentation() const
{
    ReadView view;
    GfVec4f background;
    {
        std::lock_guard lock(_mutex);
        auto const state = CapturePresentationState();
        view = state.raw;
        // A mapper may still pin an earlier camera/allocation. Never display
        // a progressive color estimate with that older geometric frame.
        if (state.legacyPixels) view.pixels = state.legacyPixels;
        if (state.snapshot) {
            if (_materializedSnapshot.lock() != state.snapshot || !_materializedPixels) {
                auto pixels = std::make_shared<std::vector<uint8_t>>(*view.pixels);
                _presentationCopyBytes.fetch_add(pixels->size(), std::memory_order_relaxed);
                for (auto const &tile : state.snapshot->tiles) {
                    if (!tile) continue;
                    const size_t rowBytes = size_t(tile->width)*sizeof(float)*4;
                    for (unsigned y=0; y<tile->height; ++y)
                        std::memcpy(pixels->data() +
                            (size_t(tile->y+y)*view.width+tile->x)*sizeof(float)*4,
                            tile->rgba.data()+size_t(y)*tile->width*4, rowBytes);
                }
                _materializedPixels = std::move(pixels);
                _materializedSnapshot = state.snapshot;
            }
            view.pixels = _materializedPixels;
        }
        if (!_hasPresentationClearColor || view.format != HdFormatFloat32Vec4 || !view.pixels)
            return view;
        background = _presentationClearColor;
        // FrameRecorder's GPU export also reads the AOV texture. Its
        // transparent clear value must keep the original RGBA there too.
        if (background[3] == 0.0f) return view;
        if (_presentationPixels && _cachedPresentationClearColor == background &&
            _presentationSource.lock() == view.pixels) {
            view.pixels = _presentationPixels;
            return view;
        }
    }
    const auto source = view.pixels;
    const float backgroundAlpha = std::clamp(background[3], 0.0f, 1.0f);
    std::shared_ptr<std::vector<uint8_t>> pixels;
    for (size_t offset = 0; offset + sizeof(float) * 4 <= view.pixels->size(); offset += sizeof(float) * 4) {
        float rgba[4];
        std::memcpy(rgba, view.pixels->data() + offset, sizeof(rgba));
        if (rgba[3] == 1.0f) continue;
        if (!pixels) {
            pixels = std::make_shared<std::vector<uint8_t>>(*view.pixels);
            _presentationCopyBytes.fetch_add(pixels->size(), std::memory_order_relaxed);
        }
        const float uncovered = 1.0f - std::clamp(rgba[3], 0.0f, 1.0f);
        for (size_t component = 0; component < 3; ++component)
            rgba[component] += background[component] * backgroundAlpha * uncovered;
        rgba[3] += backgroundAlpha * uncovered;
        std::memcpy(pixels->data() + offset, rgba, sizeof(rgba));
    }
    if (pixels) view.pixels = std::move(pixels);
    {
        std::lock_guard lock(_mutex);
        if (_hasPresentationClearColor && _presentationClearColor == background) {
            _presentationSource = source;
            _presentationPixels = view.pixels;
            _cachedPresentationClearColor = background;
        }
    }
    return view;
}

void PrismRenderBuffer::Resolve() {}

void PrismRenderBuffer::ResetSamples()
{
    std::lock_guard lock(_mutex);
    _ClearRefinement();
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
    case HdFormatInt16: {
        const float bounded = std::isnan(v) ? 0.0f :
            std::clamp(v, static_cast<float>(std::numeric_limits<int16_t>::min()),
                       static_cast<float>(std::numeric_limits<int16_t>::max()));
        reinterpret_cast<int16_t *>(dst)[c] = static_cast<int16_t>(bounded);
        break;
    }
    case HdFormatUInt16: {
        const float bounded = std::isnan(v) ? 0.0f :
            std::clamp(v, 0.0f,
                       static_cast<float>(std::numeric_limits<uint16_t>::max()));
        reinterpret_cast<uint16_t *>(dst)[c] = static_cast<uint16_t>(bounded);
        break;
    }
    case HdFormatInt32: {
        const double bounded = std::isnan(v) ? 0.0 :
            std::clamp(static_cast<double>(v),
                static_cast<double>(std::numeric_limits<int32_t>::min()),
                static_cast<double>(std::numeric_limits<int32_t>::max()));
        reinterpret_cast<int32_t *>(dst)[c] = static_cast<int32_t>(bounded);
        break;
    }
    default: break;
    }
}

static void _StoreIntComponent(HdFormat format, uint8_t *dst, size_t c, int v)
{
    switch (HdGetComponentFormat(format)) {
    case HdFormatInt16:
        reinterpret_cast<int16_t *>(dst)[c] = static_cast<int16_t>(
            std::clamp(v, static_cast<int>(std::numeric_limits<int16_t>::min()),
                       static_cast<int>(std::numeric_limits<int16_t>::max())));
        break;
    case HdFormatUInt16:
        reinterpret_cast<uint16_t *>(dst)[c] = static_cast<uint16_t>(
            std::clamp(v, 0, static_cast<int>(std::numeric_limits<uint16_t>::max())));
        break;
    case HdFormatInt32:
        reinterpret_cast<int32_t *>(dst)[c] = static_cast<int32_t>(v);
        break;
    default:
        _StoreComponent(format, dst, c, static_cast<float>(v));
        break;
    }
}

void PrismRenderBuffer::PublishPreviewFloat(
    unsigned int previewWidth, unsigned int previewHeight,
    std::vector<float> const &values, size_t components, Frame frame, bool smooth)
{
    if (previewWidth == 0 || previewHeight == 0 || components == 0 ||
        values.size() < static_cast<size_t>(previewWidth) * previewHeight *
                            components) return;
    if (_width == 0 || _height == 0 || _format == HdFormatInvalid) return;
    auto published = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(_format));
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
    if (smooth) {
        // Bilinear magnification between preview pixel centers.
        const auto source = [&](unsigned int x, unsigned int y) {
            return values.data() + (static_cast<size_t>(y) * previewWidth + x) * components;
        };
        tbb::parallel_for(0u, _height, [&](unsigned int y) {
            const float fy = std::clamp((y + 0.5f) * previewHeight / _height - 0.5f,
                                        0.0f, float(previewHeight - 1));
            const unsigned int y0 = static_cast<unsigned int>(fy);
            const unsigned int y1 = std::min(y0 + 1, previewHeight - 1);
            const float ty = fy - float(y0);
            std::vector<float> value(components);
            for (unsigned int x = 0; x < _width; ++x) {
                const float fx = std::clamp((x + 0.5f) * previewWidth / _width - 0.5f,
                                            0.0f, float(previewWidth - 1));
                const unsigned int x0 = static_cast<unsigned int>(fx);
                const unsigned int x1 = std::min(x0 + 1, previewWidth - 1);
                const float tx = fx - float(x0);
                for (size_t c = 0; c < components; ++c) {
                    const float top = std::lerp(source(x0, y0)[c], source(x1, y0)[c], tx);
                    const float bottom = std::lerp(source(x0, y1)[c], source(x1, y1)[c], tx);
                    value[c] = std::lerp(top, bottom, ty);
                }
                uint8_t *dst = published->data() + (static_cast<size_t>(y) * _width + x) * stride;
                for (size_t c = 0; c < arity; ++c)
                    _StoreComponent(_format, dst, c,
                        c < components ? value[c] : (c == 3 ? 1.0f : 0.0f));
            }
        });
        std::lock_guard lock(_mutex);
        _pixels = std::move(published);
        _frame = std::move(frame);
        _ClearRefinement();
        _hasPublished = true;
        return;
    }
    // Each preview row fills its own block of buffer rows.
    tbb::parallel_for(0u, previewHeight, [&](unsigned int py) {
        std::vector<uint8_t> expandedRow(static_cast<size_t>(_width) * stride);
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
            std::memcpy(published->data() + static_cast<size_t>(y) * _width * stride,
                        expandedRow.data(), expandedRow.size());
    });
    std::lock_guard lock(_mutex);
    _pixels = std::move(published);
    _frame = std::move(frame);
    _ClearRefinement();
    _hasPublished = true;
}

void PrismRenderBuffer::PublishPreviewInt(
    unsigned int previewWidth, unsigned int previewHeight,
    std::vector<int> const &values, size_t components, Frame frame)
{
    if (previewWidth == 0 || previewHeight == 0 || components == 0 ||
        values.size() < static_cast<size_t>(previewWidth) * previewHeight *
                            components) return;
    if (_width == 0 || _height == 0 || _format == HdFormatInvalid) return;
    auto published = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(_format));
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
            _StoreIntComponent(_format, dst, c, v);
        }
    }
    // Each preview row fills its own block of buffer rows.
    tbb::parallel_for(0u, previewHeight, [&](unsigned int py) {
        std::vector<uint8_t> expandedRow(static_cast<size_t>(_width) * stride);
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
            std::memcpy(published->data() + static_cast<size_t>(y) * _width * stride,
                        expandedRow.data(), expandedRow.size());
    });
    std::lock_guard lock(_mutex);
    _pixels = std::move(published);
    _frame = std::move(frame);
    _ClearRefinement();
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
    _Write(x, y, value, n, accumulate);
}

void PrismRenderBuffer::KeepAccumulated(unsigned int x, unsigned int y)
{
    if (x >= _width || y >= _height || _format == HdFormatInvalid) return;
    const size_t pixel = static_cast<size_t>(y) * _width + x;
    const uint32_t count = _sampleCounts[pixel];
    if (count == 0) return;
    const size_t arity = HdGetComponentCount(_format);
    uint8_t *dst = _workingPixels.data() + pixel * HdDataSizeOfFormat(_format);
    // The same value _Write stored when the pixel was last traced.
    for (size_t c = 0; c < arity; ++c)
        _StoreComponent(_format, dst, c,
                        _accumulation[pixel * arity + c] / static_cast<float>(count));
}

void PrismRenderBuffer::WriteInt(unsigned int x, unsigned int y,
                                 const int *value, size_t n)
{
    if (x >= _width || y >= _height) return;
    const size_t arity = HdGetComponentCount(_format);
    uint8_t *dst = _workingPixels.data() +
        (static_cast<size_t>(y) * _width + x) * HdDataSizeOfFormat(_format);
    for (size_t c = 0; c < arity; ++c) {
        const int v = c < n ? value[c] : 0;
        _StoreIntComponent(_format, dst, c, v);
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
    _ClearRefinement();
    auto clearWorking = [&] {
        std::fill(_accumulation.begin(), _accumulation.end(), 0.0f);
        std::fill(_sampleCounts.begin(), _sampleCounts.end(), 0);
        for (unsigned int y = 0; y < _height; ++y) for (unsigned int x = 0; x < _width; ++x) {
        if (ints) {
            uint8_t *dst = _workingPixels.data() +
                (static_cast<size_t>(y) * _width + x) * HdDataSizeOfFormat(_format);
            for (size_t c = 0; c < HdGetComponentCount(_format); ++c)
                _StoreIntComponent(_format, dst, c, c < n ? i[c] : 0);
        } else _Write(x, y, f, n, false);
        }
    };
    clearWorking();
    _pixels = std::make_shared<std::vector<uint8_t>>(_workingPixels);
    _frame.reset();
    _ClearRefinement();
    _hasPublished = true;
}

void PrismRenderBuffer::_FillWorking(VtValue const &value)
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

    const size_t stride = HdDataSizeOfFormat(_format);
    const size_t rowBytes = static_cast<size_t>(_width) * stride;
    if (rowBytes == 0 || _height == 0 || _workingPixels.size() < rowBytes * _height)
        return;
    // Encode a single pixel exactly as _Write would, then replicate it.
    const size_t arity = HdGetComponentCount(_format);
    uint8_t *row = _workingPixels.data();
    for (size_t c = 0; c < arity; ++c) {
        if (ints) {
            const int v = c < n ? i[c] : 0;
            _StoreIntComponent(_format, row, c, v);
        } else {
            _StoreComponent(_format, row, c,
                            c < n ? f[c] : (c == 3 ? 1.0f : 0.0f));
        }
    }
    for (size_t filled = stride; filled < rowBytes;) {
        const size_t count = std::min(filled, rowBytes - filled);
        std::memcpy(row + filled, row, count);
        filled += count;
    }
    for (unsigned int y = 1; y < _height; ++y)
        std::memcpy(row + static_cast<size_t>(y) * rowBytes, row, rowBytes);
}

void PrismRenderBuffer::BeginFrame(VtValue const &value, bool clearWorking)
{
    std::lock_guard lock(_mutex);
    _ClearRefinement();
    std::fill(_accumulation.begin(), _accumulation.end(), 0.0f);
    std::fill(_sampleCounts.begin(), _sampleCounts.end(), 0);
    // Every supported AOV overwrites every working pixel in the first full
    // pass.  Once a preview/complete frame exists, clearing that hidden image
    // on the UI thread is redundant and expensive at viewport resolutions.
    if (_hasPublished && !clearWorking) return;
    if (clearWorking && value.IsEmpty() && _pixels)
        _workingPixels = *_pixels;
    else
        _FillWorking(value);
    if (!_hasPublished)
        _pixels = std::make_shared<std::vector<uint8_t>>(_workingPixels);
}

bool PrismRenderBuffer::NeedsInitialClear() const
{
    std::lock_guard lock(_mutex);
    return !_hasPublished && !_initialCleared && _width && _height;
}

bool PrismRenderBuffer::ApplyInitialClear(VtValue const &value)
{
    std::lock_guard lock(_mutex);
    if (_hasPublished || _initialCleared || !_width || !_height) return false;
    _initialCleared = true;
    // A binding without a clear value asks the delegate not to clear.
    if (value.IsEmpty()) return false;
    _FillWorking(value);
    _pixels = std::make_shared<std::vector<uint8_t>>(_workingPixels);
    return true;
}

void PrismRenderBuffer::Publish(Frame frame)
{
    const auto start = std::chrono::steady_clock::now();
    auto next = std::make_shared<std::vector<uint8_t>>(std::move(_workingPixels));
    const auto size = next->size();
    std::shared_ptr<std::vector<uint8_t>> previous;
    std::shared_ptr<const PresentationSnapshot> previousPresentation;
    {
        std::lock_guard lock(_mutex);
        previous = std::exchange(_pixels, std::move(next));
        _frame = std::move(frame);
        // Releasing a pass can free thousands of tile allocations. Retire
        // those outside the publication lock so a host Map never waits for
        // tile destruction while trying to display the completed image.
        previousPresentation = std::move(_presentationSnapshot);
        _ClearRefinement();
        _hasPublished = true;
    }
    previousPresentation.reset();
    if (previous && previous.use_count() == 1)
        _workingPixels = std::move(*previous);
    else
        _workingPixels.resize(size);
    _workingPixels.resize(size);
    _publishNanoseconds.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count()),
        std::memory_order_relaxed);
}

PXR_NAMESPACE_CLOSE_SCOPE
