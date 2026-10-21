#include "texture_streaming.h"

#include <algorithm>
#include <queue>
#include <tuple>

namespace prism::device::gpu {

uint64_t StreamedLevelBytes(StreamedImage const& image, uint32_t level)
{
    if (level >= image.levels) return 0;
    const uint64_t w = std::max<uint64_t>(1, image.width >> level);
    const uint64_t h = std::max<uint64_t>(1, image.height >> level);
    return w * h * image.texelBytes;
}

uint64_t StreamedBytes(StreamedImage const& image, uint32_t finest)
{
    uint64_t bytes = 0;
    for (uint32_t l = finest; l < image.levels; ++l) bytes += StreamedLevelBytes(image, l);
    return bytes;
}

uint32_t TailLevel(uint32_t width, uint32_t height, uint32_t levels, uint32_t tailSize)
{
    uint32_t level = 0;
    while (level + 1 < levels && std::max(std::max(1u, width >> level), std::max(1u, height >> level)) > tailSize)
        ++level;
    return level;
}

void StreamingPlanner::reset(std::vector<StreamedImage> images, uint64_t budget, std::vector<uint32_t> resident,
                             double threshold)
{
    m_threshold = threshold;
    m_images.clear();
    m_images.reserve(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        State state;
        state.image = images[i];
        state.image.levels = std::max(1u, state.image.levels);
        state.image.tailLevel = std::min(state.image.tailLevel, state.image.levels - 1);
        state.resident = i < resident.size() ? std::min(resident[i], state.image.tailLevel) : state.image.tailLevel;
        m_images.push_back(state);
    }
    m_budget = budget;
    m_round = 0;
    m_accumulationRound = 0;
}

void StreamingPlanner::beginAccumulation()
{
    for (State& state : m_images) {
        state.request = kNoRequest;
        state.count = 0;
        for (uint64_t& m : state.mass) m = 0;
    }
    m_accumulationRound = m_round;
}

bool StreamingPlanner::addFeedback(uint32_t const* histograms, size_t count)
{
    ++m_round;
    bool finer = false;
    const size_t n = std::min(count, m_images.size());
    for (size_t i = 0; i < n; ++i) {
        uint32_t const* bins = histograms + i * kFeedbackLevels;
        State& state = m_images[i];
        uint64_t added = 0;
        for (uint32_t l = 0; l < kFeedbackLevels; ++l) {
            if (!bins[l]) continue;
            state.mass[std::min(l, state.image.levels - 1)] += bins[l];
            added += bins[l];
        }
        if (!added) continue;
        state.count += added;
        state.lastRequested = m_round;
        // The coarsest level that leaves out at most `threshold` of the weight.
        const double allowed = m_threshold * double(state.count);
        uint64_t finerMass = 0;
        uint32_t level = 0;
        while (level + 1 < state.image.levels && double(finerMass + state.mass[level]) <= allowed)
            finerMass += state.mass[level++];
        // Requests at the tail or coarser need nothing streamed, but still
        // mark the image as in use (eviction order).
        if (level < std::min(state.request, state.image.tailLevel)) finer = true;
        state.request = level;
    }
    return finer;
}

std::vector<uint32_t> StreamingPlanner::plan() const
{
    const size_t n = m_images.size();
    std::vector<uint32_t> target(n);
    uint64_t used = 0;
    // 1. Tails, and levels resident and requested in this accumulation.
    for (size_t i = 0; i < n; ++i) {
        State const& s = m_images[i];
        uint32_t level = s.image.tailLevel;
        if (s.request != kNoRequest) level = std::min(level, std::max(s.resident, s.request));
        target[i] = level;
        used += StreamedBytes(s.image, level);
    }
    // 2. Requested levels, one at a time, by request count per byte.
    using Step = std::tuple<double, uint64_t, uint32_t>; // priority, ~image (ties: lower index first), level
    std::priority_queue<Step> steps;
    auto push = [&](size_t i) {
        State const& s = m_images[i];
        if (s.request == kNoRequest || target[i] == 0 || target[i] - 1 < s.request) return;
        const uint32_t level = target[i] - 1;
        const double priority = double(s.count + 1) / double(std::max<uint64_t>(1, StreamedLevelBytes(s.image, level)));
        steps.push({priority, ~uint64_t(i), level});
    };
    for (size_t i = 0; i < n; ++i) push(i);
    while (!steps.empty()) {
        const auto [priority, key, level] = steps.top();
        steps.pop();
        const size_t i = size_t(~key);
        const uint64_t bytes = StreamedLevelBytes(m_images[i].image, level);
        if (used + bytes > m_budget) continue; // its finer levels cost more still
        used += bytes;
        target[i] = level;
        push(i);
    }
    // 3. Other resident levels while they fit, most recently requested first.
    std::vector<size_t> keep;
    for (size_t i = 0; i < n; ++i)
        if (m_images[i].resident < target[i]) keep.push_back(i);
    std::stable_sort(keep.begin(), keep.end(), [&](size_t a, size_t b) {
        return m_images[a].lastRequested > m_images[b].lastRequested;
    });
    for (size_t i : keep) {
        State const& s = m_images[i];
        while (target[i] > s.resident) {
            const uint64_t bytes = StreamedLevelBytes(s.image, target[i] - 1);
            if (used + bytes > m_budget) break;
            used += bytes;
            --target[i];
        }
    }
    return target;
}

void StreamingPlanner::setResident(size_t image, uint32_t finest)
{
    if (image < m_images.size()) m_images[image].resident = std::min(finest, m_images[image].image.tailLevel);
}

StreamingPlanner::Stats StreamingPlanner::stats(std::vector<uint32_t> const& plan) const
{
    Stats stats;
    stats.budget = m_budget;
    stats.images = uint32_t(m_images.size());
    for (size_t i = 0; i < m_images.size(); ++i) {
        State const& s = m_images[i];
        stats.residentBytes += StreamedBytes(s.image, s.resident);
        const uint32_t planned = i < plan.size() ? plan[i] : s.resident;
        stats.plannedBytes += StreamedBytes(s.image, planned);
        if (s.request != kNoRequest && s.request < s.image.tailLevel) {
            ++stats.requested;
            if (planned > s.request) ++stats.belowRequested;
        }
        if (planned < s.resident) ++stats.pending;
    }
    return stats;
}

} // namespace prism::device::gpu
