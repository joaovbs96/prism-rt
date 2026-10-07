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
        PrismRenderBuffer presentation(SdfPath("/presentation"), nullptr);
        presentation.Allocate(GfVec3i(3, 1, 1), HdFormatFloat32Vec4, false);
        const float miss[4] = {0, 0, 0, 0};
        const float edge[4] = {.125f, .25f, .375f, .5f};
        const float opaque[4] = {2, 3, 4, 1};
        presentation.WriteFloat(0, 0, miss, 4, false);
        presentation.WriteFloat(1, 0, edge, 4, false);
        presentation.WriteFloat(2, 0, opaque, 4, false);
        presentation.Publish(frame);
        auto raw = presentation.Capture();
        const auto original = *raw.pixels;
        presentation.SetPresentationClearColor(VtValue(GfVec4f(.25f, .5f, .75f, 1)));
        auto viewport = presentation.CapturePresentation();
        const float *shown = reinterpret_cast<float const *>(viewport.pixels->data());
        const float expected[12] = {.25f, .5f, .75f, 1, .25f, .5f, .75f, 1, 2, 3, 4, 1};
        if (!std::equal(shown, shown + 12, expected) || viewport.frame != frame ||
            *presentation.Capture().pixels != original)
            throw std::runtime_error("viewport background changed raw RGBA or mishandled premultiplied edges");
        if (presentation.CapturePresentation().pixels != viewport.pixels)
            throw std::runtime_error("unchanged viewport upload rebuilt its presentation copy");
        const float *exported = static_cast<float const *>(presentation.Map());
        if (!std::equal(exported, exported + 4, miss) || !std::equal(exported + 4, exported + 8, edge))
            throw std::runtime_error("rendered-image mapping contains the viewport background");
        presentation.Unmap();
        presentation.SetPresentationClearColor(VtValue(GfVec4f(.5f, .25f, 0, 1)));
        auto changedBackground = presentation.CapturePresentation();
        const float *changed = reinterpret_cast<float const *>(changedBackground.pixels->data());
        if (changed[0] != .5f || changed[1] != .25f || changed[2] != 0 ||
            *presentation.Capture().pixels != original || !std::equal(shown, shown + 12, expected))
            throw std::runtime_error("clear color update changed raw pixels or an immutable presentation copy");
        presentation.SetPresentationClearColor(VtValue(GfVec4f(.25f, .5f, .75f, 0)));
        if (presentation.CapturePresentation().pixels != raw.pixels)
            throw std::runtime_error("transparent GPU export clear changed raw RGBA upload");
        presentation.SetPresentationClearColor(VtValue(GfVec4f(.25f, .5f, .75f, .5f)));
        auto translucent = presentation.CapturePresentation();
        const float *partial = reinterpret_cast<float const *>(translucent.pixels->data());
        if (partial[0] != .125f || partial[3] != .5f || partial[4] != .1875f || partial[7] != .75f)
            throw std::runtime_error("presentation upload ignored clear color alpha");
        presentation.SetPresentationClearColor(VtValue());
        if (presentation.CapturePresentation().pixels != raw.pixels)
            throw std::runtime_error("non-color AOV was composited with the background");
        auto refinement = std::make_shared<std::vector<uint8_t>>(original);
        float *refined = reinterpret_cast<float *>(refinement->data());
        refined[0] = .125f; refined[1] = .25f; refined[2] = .375f; refined[3] = .5f;
        presentation.SetPresentationPixels(refinement, frame);
        presentation.SetPresentationClearColor(VtValue(GfVec4f(0)));
        if (presentation.CapturePresentation().pixels != refinement ||
            *presentation.Capture().pixels != original ||
            presentation.CapturePublished().pixels != raw.pixels)
            throw std::runtime_error("display refinement changed committed raw publication");
        exported = static_cast<float const *>(presentation.Map());
        if (!std::equal(exported, exported + 4, miss))
            throw std::runtime_error("display refinement changed raw export mapping");
        presentation.Unmap();
        presentation.SetPresentationClearColor(VtValue(GfVec4f(.25f, .5f, .75f, 1)));
        auto refinedBackground = presentation.CapturePresentation();
        const float *composed = reinterpret_cast<float const *>(refinedBackground.pixels->data());
        if (composed[0] != .25f || composed[1] != .5f || composed[2] != .75f || composed[3] != 1)
            throw std::runtime_error("viewport background ignored the progressive display pixels");
        presentation.SetPresentationClearColor(VtValue(GfVec4f(0)));
        auto heldRefinement = presentation.CapturePresentation();
        presentation.ClearPresentationPixels();
        if (presentation.CapturePresentation().pixels != raw.pixels ||
            heldRefinement.pixels != refinement || (*heldRefinement.pixels)[0] != (*refinement)[0])
            throw std::runtime_error("cancellation changed raw pixels or held immutable refinement");
        presentation.SetPresentationPixels(refinement, frame);
        presentation.Publish(frame);
        if (presentation.CapturePresentation().pixels != presentation.Capture().pixels)
            throw std::runtime_error("complete sample publication retained a partial override");
        auto oldPinned = presentation.Capture();
        presentation.Map();
        auto nextFrame = std::make_shared<prism::hydra::PublishedFrame>();
        nextFrame->generation = 8;
        presentation.WriteFloat(0, 0, opaque, 4, false);
        presentation.Publish(nextFrame);
        auto newest = presentation.CapturePublished();
        auto nextRefinement = std::make_shared<std::vector<uint8_t>>(*newest.pixels);
        presentation.SetPresentationPixels(nextRefinement, nextFrame);
        auto pinnedPresentation = presentation.CapturePresentation();
        if (pinnedPresentation.frame != oldPinned.frame || pinnedPresentation.pixels != oldPinned.pixels ||
            presentation.CapturePublished().frame != nextFrame)
            throw std::runtime_error("new-camera refinement replaced an older mapped geometric frame");
        presentation.Unmap();
        if (presentation.CapturePresentation().pixels != nextRefinement)
            throw std::runtime_error("latest refinement did not appear after releasing old frame mapping");
        presentation.ResetSamples();
        if (presentation.CapturePresentation().pixels != newest.pixels)
            throw std::runtime_error("accumulation reset retained partial display pixels");
        presentation.SetPresentationPixels(nextRefinement, nextFrame);
        presentation.Allocate(GfVec3i(1, 1, 1), HdFormatFloat32Vec4, false);
        presentation.SetPresentationPixels(nextRefinement, nextFrame);
        auto resizedPresentation = presentation.CapturePresentation();
        if (resizedPresentation.width != 1 || resizedPresentation.frame ||
            resizedPresentation.pixels != presentation.CapturePublished().pixels)
            throw std::runtime_error("resize accepted stale refinement dimensions or frame metadata");
        {
            PrismRenderBuffer tiled(SdfPath("/tiled"),nullptr);
            tiled.Allocate(GfVec3i(65,35,1),HdFormatFloat32Vec4,false);
            tiled.Clear(VtValue(GfVec4f(0)));
            tiled.Publish(frame);
            const auto base=tiled.CapturePublished();
            auto tile=std::make_shared<PrismRenderBuffer::PresentationTile>();
            tile->x=64; tile->y=32; tile->width=1; tile->height=3;
            tile->rgba={.1f,.2f,.3f,.5f,.4f,.5f,.6f,.75f,.7f,.8f,.9f,1};
            auto packet=std::make_shared<PrismRenderBuffer::PresentationSnapshot>();
            packet->base=base; packet->pass=0; packet->revision=1;
            packet->tiles.resize(9); packet->tiles[8]=tile;
            tiled.SetPresentationSnapshot(packet);
            auto state=tiled.CapturePresentationState();
            if (state.snapshot!=packet || state.raw.pixels!=base.pixels ||
                tiled.GetPresentationCopyBytes()!=0)
                throw std::runtime_error("native tile capture materialized or changed raw storage");
            const auto materialized=tiled.CapturePresentation();
            auto const *values=reinterpret_cast<float const *>(materialized.pixels->data());
            for(unsigned y=0;y<35;++y) for(unsigned x=0;x<65;++x) {
                const size_t offset=(size_t(y)*65+x)*4;
                for(size_t c=0;c<4;++c) {
                    float expected=x==64 && y>=32 ? tile->rgba[(y-32)*4+c] : 0;
                    if(values[offset+c]!=expected)
                        throw std::runtime_error("tile materialization changed edges or row orientation");
                }
            }
            const uint64_t copyBytes=tiled.GetPresentationCopyBytes();
            if (copyBytes!=size_t(65)*35*4*sizeof(float) ||
                tiled.CapturePresentation().pixels!=materialized.pixels ||
                tiled.GetPresentationCopyBytes()!=copyBytes)
                throw std::runtime_error("unchanged tile packet was copied more than once");
            auto newer=std::make_shared<PrismRenderBuffer::PresentationSnapshot>(*packet);
            newer->revision=3;
            auto added=std::make_shared<PrismRenderBuffer::PresentationTile>();
            added->x=0; added->y=0; added->width=1; added->height=1;
            added->rgba={1,0,0,1}; newer->tiles[0]=added;
            tiled.SetPresentationSnapshot(newer);
            tiled.SetPresentationSnapshot(packet);
            if (tiled.CapturePresentationState().snapshot!=newer || values[0]!=0)
                throw std::runtime_error("old tile revision replaced or mutated an immutable packet");
            auto malformed=std::make_shared<PrismRenderBuffer::PresentationSnapshot>(*newer);
            malformed->revision=4;
            auto outside=std::make_shared<PrismRenderBuffer::PresentationTile>(*added);
            outside->x=65; malformed->tiles[0]=outside;
            tiled.SetPresentationSnapshot(malformed);
            if (tiled.CapturePresentationState().snapshot!=newer)
                throw std::runtime_error("out-of-bounds tile packet was accepted");
            auto const *mapped=static_cast<float const *>(tiled.Map());
            if (mapped[0]!=0) throw std::runtime_error("tile presentation changed raw Map export");
            tiled.WriteFloat(0,0,opaque,4,false);
            tiled.Publish(frame); // Same camera/frame, different sample publication.
            auto nextPacket=std::make_shared<PrismRenderBuffer::PresentationSnapshot>(*newer);
            nextPacket->base=tiled.CapturePublished(); nextPacket->pass=1;
            tiled.SetPresentationSnapshot(nextPacket);
            if (tiled.CapturePresentationState().snapshot || tiled.CapturePresentation().pixels!=base.pixels)
                throw std::runtime_error("new-sample tiles were applied to a pinned older raw sample");
            tiled.Unmap();
            if(tiled.CapturePresentationState().snapshot!=nextPacket)
                throw std::runtime_error("latest tile packet missing after unmap");
            tiled.ClearPresentationPixels();
            if(tiled.CapturePresentationState().snapshot ||
               tiled.CapturePresentation().pixels!=nextPacket->base.pixels || values[0]!=0)
                throw std::runtime_error("tile cancellation changed a retained immutable view");
            tiled.SetPresentationSnapshot(packet);
            if(tiled.CapturePresentationState().snapshot)
                throw std::runtime_error("stale raw-base tile packet accepted");
            tiled.SetPresentationSnapshot(nextPacket);
            tiled.Allocate(GfVec3i(2,2,1),HdFormatFloat32Vec4,false);
            tiled.SetPresentationSnapshot(nextPacket);
            if(tiled.CapturePresentationState().snapshot)
                throw std::runtime_error("resize retained incompatible tile packet");
        }
        std::cout << "Mapped storage survives publication, nested maps, and resize\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
