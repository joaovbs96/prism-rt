#pragma once

#include "pxr/imaging/hd/renderBuffer.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace prism::hydra { struct PublishedFrame; }

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderThread;

class PrismRenderBuffer final : public HdRenderBuffer
{
public:
    PrismRenderBuffer(SdfPath const &id, HdRenderThread *renderThread);
    ~PrismRenderBuffer() override;

    bool Allocate(GfVec3i const &dimensions, HdFormat format,
                  bool multiSampled) override;
    unsigned int GetWidth() const override { return _width; }
    unsigned int GetHeight() const override { return _height; }
    unsigned int GetDepth() const override { return 1; }
    HdFormat GetFormat() const override { return _format; }
    bool IsMultiSampled() const override { return _multiSampled; }
    void *Map() override;
    void Unmap() override;
    bool IsMapped() const override { return _mappers.load() != 0; }
    void Resolve() override;
    bool IsConverged() const override { return _converged.load(); }

    void Clear(VtValue const &value);
    // Start a new accumulation without exposing a partially written frame.
    // The previous published pixels remain visible until Publish() is called.
    // Once a frame exists, callers must overwrite every working pixel before
    // publishing; avoiding a redundant full-resolution clear keeps camera
    // interaction responsive.
    void BeginFrame(VtValue const &clearValue);
    // True until the buffer has shown a frame or received its initial clear
    // since it was last allocated.
    bool NeedsInitialClear() const;
    // Fills a not-yet-shown buffer with its AOV clear value so the host never
    // presents the zero-filled allocation. The caller must have stopped the
    // render worker. Does nothing once a frame exists, and only acts once per
    // allocation, so it never touches a displayed image or a running render.
    bool ApplyInitialClear(VtValue const &clearValue);
    using Frame = std::shared_ptr<const prism::hydra::PublishedFrame>;
    struct ReadView {
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        Frame frame;
        unsigned int width=0,height=0;
        HdFormat format=HdFormatInvalid;
    };
    // Pixels and their picking/camera metadata are one immutable publication.
    // The view stays valid through later publication and buffer allocation.
    ReadView Capture() const;
    void Publish(Frame frame = {});
    void PublishPreviewFloat(unsigned int previewWidth,
                             unsigned int previewHeight,
                             std::vector<float> const &values,
                             size_t components, Frame frame = {});
    void PublishPreviewInt(unsigned int previewWidth,
                           unsigned int previewHeight,
                           std::vector<int> const &values,
                           size_t components, Frame frame = {});
    void WriteFloat(unsigned int x, unsigned int y, const float *value,
                    size_t components, bool accumulate);
    void WriteInt(unsigned int x, unsigned int y, const int *value,
                  size_t components);
    void SetConverged(bool value) { _converged.store(value); }
    void ResetSamples();
    uint64_t GetWriteNanoseconds() const { return _writeNanoseconds.load(); }
    // Rendering workers measure a complete tile locally and update this
    // counter once per tile.  This keeps profiling out of the per-pixel fast
    // path and avoids making the counter a shared cache-line bottleneck.
    void RecordWriteNanoseconds(uint64_t value)
    {
        _writeNanoseconds.fetch_add(value, std::memory_order_relaxed);
    }
    uint64_t GetPublishNanoseconds() const { return _publishNanoseconds.load(); }
    uint64_t GetMapWaitNanoseconds() const { return _mapWaitNanoseconds.load(); }

protected:
    void _Deallocate() override;

private:
    void _Write(unsigned int x, unsigned int y, const float *value,
                size_t components, bool accumulate);
    // Replicates one encoded clear pixel over the whole working image.
    void _FillWorking(VtValue const &clearValue);

    unsigned int _width = 0;
    unsigned int _height = 0;
    HdFormat _format = HdFormatInvalid;
    bool _multiSampled = false;
    // Mappers pin one published generation until the last Unmap. Publication
    // never modifies that generation or waits for an application to unmap it.
    std::shared_ptr<std::vector<uint8_t>> _pixels;
    std::shared_ptr<std::vector<uint8_t>> _mappedPixels;
    Frame _frame, _mappedFrame;
    unsigned int _mappedWidth=0,_mappedHeight=0;
    HdFormat _mappedFormat=HdFormatInvalid;
    std::vector<uint8_t> _workingPixels;
    std::vector<float> _accumulation;
    std::vector<uint32_t> _sampleCounts;
    mutable std::recursive_mutex _mutex;
    std::atomic<int> _mappers{0};
    std::atomic<bool> _converged{false};
    std::atomic<uint64_t> _writeNanoseconds{0};
    std::atomic<uint64_t> _publishNanoseconds{0};
    std::atomic<uint64_t> _mapWaitNanoseconds{0};
    bool _hasPublished = false;
    bool _initialCleared = false;
    HdRenderThread *_renderThread;
};

PXR_NAMESPACE_CLOSE_SCOPE
