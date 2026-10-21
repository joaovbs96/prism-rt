#pragma once

// Texture streaming at whole-mip-level granularity: which levels of each
// streamed image a GPU device keeps resident, from the levels shading asked
// for (texture feedback), within a byte budget. Independent of the graphics
// API; the Vulkan device applies the plans (vulkan_device.cpp).
//
// Each image keeps a contiguous range of levels resident, [finest, coarsest],
// and never less than its tail (the levels at most PRISM_GPU_STREAMING_TAIL
// texels on a side), so every lookup has something to sample. Feedback is a
// histogram per image of the levels lookups asked for, each weighted by its
// path's throughput, over the current accumulation (a camera or scene change
// starts a new one). An image's request is the coarsest level that serves
// all but a fraction `threshold` of that weight: levels only negligible
// paths ask for are left out. That is a bounded bias in filter footprint
// (those lookups read a blurrier level), never in radiance. Planning:
//
// 1. Tails always; levels both resident and requested in this accumulation
//    stay (never demoted while the image converges).
// 2. Requested levels not yet resident, one level at a time, by importance
//    per byte (request count / level bytes, so coarse levels of much-used
//    textures first), as long as each fits the budget.
// 3. Other resident levels (requested earlier, or finer than needed) stay
//    while the budget allows, most recently requested first: the least
//    recently requested are evicted.
//
// Plans are deterministic for the same feedback. Tiles would extend a level
// to per-tile residency; the budget and priority rules stay.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace prism::device::gpu {

constexpr uint32_t kNoRequest = 0xffffffffu;
constexpr uint32_t kFeedbackLevels = 16; // histogram bins per image (accessors.hlsl)

struct StreamedImage {
    uint32_t width = 0, height = 0; // level 0
    uint32_t levels = 1;            // full chain
    uint32_t texelBytes = 4;        // as stored on the GPU
    uint32_t tailLevel = 0;         // finest level of the always resident tail
};

uint64_t StreamedLevelBytes(StreamedImage const& image, uint32_t level);
// Bytes of levels [finest, levels).
uint64_t StreamedBytes(StreamedImage const& image, uint32_t finest);
// The finest level whose larger side is at most `tailSize` (0 for small images).
uint32_t TailLevel(uint32_t width, uint32_t height, uint32_t levels, uint32_t tailSize);

class StreamingPlanner {
public:
    struct Stats {
        uint64_t budget = 0;
        uint64_t residentBytes = 0;  // of the streamed images, tails included
        uint64_t plannedBytes = 0;
        uint32_t images = 0;
        uint32_t requested = 0;      // images requested finer than their tail
        uint32_t belowRequested = 0; // planned coarser than requested (budget)
        uint32_t pending = 0;        // planned finer than resident
    };

    // `resident` (optional): each image's finest resident level (default its
    // tail). `threshold`: the fraction of an image's lookup weight its
    // request may leave out.
    void reset(std::vector<StreamedImage> images, uint64_t budget, std::vector<uint32_t> resident = {},
               double threshold = 0.001);
    // A new accumulation: requests start over; earlier ones only order eviction.
    void beginAccumulation();
    // One feedback round: per image kFeedbackLevels weights, one per level
    // asked for. True when some image's request became finer than before in
    // this accumulation.
    bool addFeedback(uint32_t const* histograms, size_t count);
    // The finest level each image should keep resident. `fromRequests`:
    // from the requests alone (steps 2 only: no levels kept for being
    // resident), so the plan does not depend on what earlier uploads made
    // resident when; settling uses it for deterministic residency.
    std::vector<uint32_t> plan(bool fromRequests = false) const;
    void setResident(size_t image, uint32_t finest);

    size_t size() const { return m_images.size(); }
    StreamedImage const& image(size_t i) const { return m_images[i].image; }
    uint32_t resident(size_t i) const { return m_images[i].resident; }
    // This accumulation's request (kNoRequest: none).
    uint32_t request(size_t i) const { return m_images[i].request; }
    uint64_t budget() const { return m_budget; }
    uint64_t round() const { return m_round; }
    Stats stats(std::vector<uint32_t> const& plan) const;

private:
    struct State {
        StreamedImage image;
        uint32_t resident = 0;
        uint32_t request = kNoRequest; // this accumulation
        uint64_t count = 0;            // this accumulation's lookup weight
        uint64_t mass[kFeedbackLevels] = {}; // ... by level asked for
        uint64_t lastRequested = 0;    // round + 1 of the last request (0: never)
    };
    std::vector<State> m_images;
    uint64_t m_budget = 0;
    double m_threshold = 0.001;
    uint64_t m_round = 0;
    uint64_t m_accumulationRound = 0; // round at which this accumulation began
};

} // namespace prism::device::gpu
