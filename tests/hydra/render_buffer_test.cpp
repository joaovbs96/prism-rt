#include "../../src/hydra/render_buffer.h"
#include "../../src/hydra/scene_bridge.h"

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <future>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

int main() {
    try {
        PrismRenderBuffer buffer(SdfPath("/color"), nullptr);
        auto frame=std::make_shared<prism::hydra::PublishedFrame>();
        frame->generation=7;
        if (!buffer.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32, false)) return 1;
        buffer.Clear(VtValue(1.0f));
        const auto* first = static_cast<const float*>(buffer.Map());
        auto publish = std::async(std::launch::async, [&] {
            const float value = 2;
            for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x)
                buffer.WriteFloat(x,y,&value,1,false);
            buffer.Publish(frame);
        });
        if (publish.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            buffer.Unmap();
            throw std::runtime_error("publication blocked on a mapped image");
        }
        publish.get();
        if (first[0]!=1 || first[3]!=1) throw std::runtime_error("mapped generation was overwritten");
        if(buffer.Capture().frame) throw std::runtime_error("mapped metadata changed during publication");
        // Nested mappings pin the same generation until all users release it.
        if (buffer.Map()!=first) throw std::runtime_error("nested mapping changed generation");
        buffer.Unmap();
        buffer.Unmap();
        auto* second = static_cast<const float*>(buffer.Map());
        const auto retained=buffer.Capture();
        if(retained.frame!=frame || retained.width!=2 || retained.height!=2)
            throw std::runtime_error("published metadata does not match pixels");
        if(second[0]!=2 || second[3]!=2) throw std::runtime_error("publication missing");
        buffer.Allocate(GfVec3i(3,1,1),HdFormatFloat32,false);
        if(buffer.Capture().width!=2 || buffer.Capture().frame!=frame)
            throw std::runtime_error("resize changed mapped metadata");
        if(second[3]!=2) throw std::runtime_error("resize invalidated mapping");
        buffer.Unmap();
        if(buffer.Capture().width!=3 || buffer.Capture().frame)
            throw std::runtime_error("allocation retained stale metadata");
        if(retained.frame->generation!=7 || reinterpret_cast<float const*>(retained.pixels->data())[3]!=2)
            throw std::runtime_error("captured view did not retain frame lifetime");
        buffer.PublishPreviewFloat(1,1,{3.0f},1);
        auto* preview=static_cast<const float*>(buffer.Map());
        if(preview[0]!=3 || preview[2]!=3) throw std::runtime_error("preview expansion failed");
        buffer.Unmap();
        // A buffer that has not shown a frame holds its clear value in every
        // pixel, including after a resize, and a shown frame is never cleared.
        {
            auto allEqual = [](PrismRenderBuffer &b, const float *expected,
                               size_t components) {
                const auto *data = static_cast<const float *>(b.Map());
                bool same = data != nullptr;
                const size_t pixels = static_cast<size_t>(b.GetWidth()) * b.GetHeight();
                for (size_t p = 0; same && p < pixels; ++p)
                    for (size_t c = 0; c < components; ++c)
                        same &= data[p * components + c] == expected[c];
                b.Unmap();
                return same;
            };
            PrismRenderBuffer color(SdfPath("/clearColor"), nullptr);
            PrismRenderBuffer depth(SdfPath("/clearDepth"), nullptr);
            PrismRenderBuffer ids(SdfPath("/clearIds"), nullptr);
            if (!color.Allocate(GfVec3i(5, 3, 1), HdFormatFloat32Vec4, false) ||
                !depth.Allocate(GfVec3i(5, 3, 1), HdFormatFloat32, false) ||
                !ids.Allocate(GfVec3i(5, 3, 1), HdFormatInt32, false))
                throw std::runtime_error("clear allocation failed");
            if (!color.NeedsInitialClear())
                throw std::runtime_error("fresh buffer does not need a clear");
            const float colorClear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            const float depthClear[1] = {1.0f};
            if (!color.ApplyInitialClear(VtValue(GfVec4f(0.25f, 0.5f, 0.75f, 1.0f))) ||
                !depth.ApplyInitialClear(VtValue(1.0f)) ||
                !ids.ApplyInitialClear(VtValue(-1)))
                throw std::runtime_error("initial clear was not applied");
            if (!allEqual(color, colorClear, 4) || !allEqual(depth, depthClear, 1))
                throw std::runtime_error("clear value missing from a float pixel");
            const auto *idData = static_cast<const int *>(ids.Map());
            bool idsCleared = idData != nullptr;
            for (size_t p = 0; idsCleared && p < 15; ++p) idsCleared &= idData[p] == -1;
            ids.Unmap();
            if (!idsCleared) throw std::runtime_error("clear value missing from an id pixel");
            if (color.NeedsInitialClear() ||
                color.ApplyInitialClear(VtValue(GfVec4f(9.0f))))
                throw std::runtime_error("buffer was cleared twice");

            // A published frame is never overwritten by a later clear.
            const float shown[4] = {0.5f, 0.25f, 0.125f, 1.0f};
            for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 5; ++x)
                color.WriteFloat(x, y, shown, 4, false);
            color.Publish();
            if (color.ApplyInitialClear(VtValue(GfVec4f(0.25f, 0.5f, 0.75f, 1.0f))) ||
                !allEqual(color, shown, 4))
                throw std::runtime_error("clear touched a published frame");
            PrismRenderBuffer previewed(SdfPath("/clearPreview"), nullptr);
            previewed.Allocate(GfVec3i(4, 4, 1), HdFormatFloat32Vec4, false);
            previewed.PublishPreviewFloat(1, 1, {0.5f, 0.5f, 0.5f, 1.0f}, 4);
            const float previewValue[4] = {0.5f, 0.5f, 0.5f, 1.0f};
            if (previewed.NeedsInitialClear() ||
                previewed.ApplyInitialClear(VtValue(GfVec4f(0.0f))) ||
                !allEqual(previewed, previewValue, 4))
                throw std::runtime_error("clear touched a preview frame");

            // Reallocation starts over: zeros are replaced by the clear value.
            color.Allocate(GfVec3i(7, 2, 1), HdFormatFloat32Vec4, false);
            const float zeros[4] = {0, 0, 0, 0};
            if (!allEqual(color, zeros, 4) || !color.NeedsInitialClear())
                throw std::runtime_error("reallocated buffer is not pending a clear");
            color.ApplyInitialClear(VtValue(GfVec4f(0.25f, 0.5f, 0.75f, 1.0f)));
            if (!allEqual(color, colorClear, 4))
                throw std::runtime_error("resized buffer was presented zero-filled");
            depth.Allocate(GfVec3i(2, 6, 1), HdFormatFloat32, false);
            depth.ApplyInitialClear(VtValue(1.0f));
            if (!allEqual(depth, depthClear, 1))
                throw std::runtime_error("resized depth buffer was not cleared");
        }
        auto checkIntegerFormat = [&]<typename Component>(
            HdFormat format, size_t components) {
            PrismRenderBuffer integerBuffer(SdfPath("/integer"), nullptr);
            if (!integerBuffer.Allocate(GfVec3i(2, 1, 1), format, false))
                throw std::runtime_error("integer format allocation failed");
            const int lowInput[4] = {
                std::numeric_limits<int>::min(), -17, 40000, -40000};
            const int highInput[4] = {
                std::numeric_limits<int>::max(), 17, 90000, 23};
            integerBuffer.WriteInt(0, 0, lowInput, components);
            integerBuffer.WriteInt(1, 0, highInput, components);
            integerBuffer.Publish();
            integerBuffer.Resolve();
            const Component *pixels = static_cast<Component const *>(integerBuffer.Map());
            if (!pixels) throw std::runtime_error("integer mapping failed");
            for (size_t c = 0; c < components; ++c) {
                const long long low = std::clamp<long long>(
                    lowInput[c], std::numeric_limits<Component>::lowest(),
                    std::numeric_limits<Component>::max());
                const long long high = std::clamp<long long>(
                    highInput[c], std::numeric_limits<Component>::lowest(),
                    std::numeric_limits<Component>::max());
                if (pixels[c] != static_cast<Component>(low) ||
                    pixels[components + c] != static_cast<Component>(high))
                    throw std::runtime_error("integer write lost or misclamped a component");
            }
            integerBuffer.Unmap();

            integerBuffer.Clear(VtValue(-3));
            const Component *cleared = static_cast<Component const *>(integerBuffer.Map());
            if (!cleared) throw std::runtime_error("integer clear mapping failed");
            for (size_t i = 0; i < 2 * components; ++i) {
                const long long expected = std::clamp<long long>(
                    i % components == 0 ? -3 : 0,
                    std::numeric_limits<Component>::lowest(),
                    std::numeric_limits<Component>::max());
                if (cleared[i] != static_cast<Component>(expected))
                    throw std::runtime_error("integer clear used the wrong component width");
            }
            integerBuffer.Unmap();

            integerBuffer.PublishPreviewInt(1, 1,
                std::vector<int>(lowInput, lowInput + components), components);
            const Component *preview = static_cast<Component const *>(integerBuffer.Map());
            if (!preview) throw std::runtime_error("integer preview mapping failed");
            for (size_t c = 0; c < components; ++c) {
                const long long expected = std::clamp<long long>(
                    lowInput[c], std::numeric_limits<Component>::lowest(),
                    std::numeric_limits<Component>::max());
                if (preview[c] != static_cast<Component>(expected) ||
                    preview[components + c] != static_cast<Component>(expected))
                    throw std::runtime_error("integer preview lost or misclamped a component");
            }
            integerBuffer.Unmap();
        };
        checkIntegerFormat.template operator()<int16_t>(HdFormatInt16, 1);
        checkIntegerFormat.template operator()<int16_t>(HdFormatInt16Vec2, 2);
        checkIntegerFormat.template operator()<int16_t>(HdFormatInt16Vec3, 3);
        checkIntegerFormat.template operator()<int16_t>(HdFormatInt16Vec4, 4);
        checkIntegerFormat.template operator()<uint16_t>(HdFormatUInt16, 1);
        checkIntegerFormat.template operator()<uint16_t>(HdFormatUInt16Vec2, 2);
        checkIntegerFormat.template operator()<uint16_t>(HdFormatUInt16Vec3, 3);
        checkIntegerFormat.template operator()<uint16_t>(HdFormatUInt16Vec4, 4);
        checkIntegerFormat.template operator()<int32_t>(HdFormatInt32, 1);
        checkIntegerFormat.template operator()<int32_t>(HdFormatInt32Vec2, 2);
        checkIntegerFormat.template operator()<int32_t>(HdFormatInt32Vec3, 3);
        checkIntegerFormat.template operator()<int32_t>(HdFormatInt32Vec4, 4);

        PrismRenderBuffer multisampled(SdfPath("/multisampled"), nullptr);
        if (!multisampled.Allocate(GfVec3i(1, 1, 1), HdFormatInt32, true) ||
            !multisampled.IsMultiSampled())
            throw std::runtime_error("multisampled allocation metadata was lost");
        const int largeId = 16777217;
        multisampled.WriteInt(0, 0, &largeId, 1);
        multisampled.Publish();
        multisampled.Resolve();
        const int *resolved = static_cast<int const *>(multisampled.Map());
        if (!resolved || resolved[0] != largeId)
            throw std::runtime_error("Resolve did not expose the published single-image result");
        multisampled.Unmap();

        PrismRenderBuffer floatToInt(SdfPath("/floatToInt"), nullptr);
        if (!floatToInt.Allocate(GfVec3i(1, 1, 1), HdFormatInt32Vec4, false))
            throw std::runtime_error("float-to-integer allocation failed");
        const float exceptionalValues[4] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), 123.75f};
        floatToInt.WriteFloat(0, 0, exceptionalValues, 4, false);
        floatToInt.Publish();
        const int *converted = static_cast<int const *>(floatToInt.Map());
        if (!converted || converted[0] != 0 ||
            converted[1] != std::numeric_limits<int32_t>::max() ||
            converted[2] != std::numeric_limits<int32_t>::min() ||
            converted[3] != 123)
            throw std::runtime_error("float-to-int conversion was not bounded");
        floatToInt.Unmap();

        PrismRenderBuffer unsupported(SdfPath("/unsupported"), nullptr);
        if (unsupported.Allocate(GfVec3i(1, 1, 1), HdFormatFloat32UInt8, false))
            throw std::runtime_error("unsupported packed format was allocated");
        std::cout << "Mapped storage survives publication, nested maps, and resize\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
