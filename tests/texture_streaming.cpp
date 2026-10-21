// The texture streaming planner (src/device/gpu/texture_streaming.h):
// budgeting, priority between requests, levels kept while an image
// converges, and least-recently-requested eviction. No GPU needed.

#include "../src/device/gpu/texture_streaming.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace prism::device::gpu;

namespace {

int failures = 0;

void check(bool condition, std::string const& what)
{
    if (condition) return;
    std::cerr << "FAILED: " << what << '\n';
    ++failures;
}

// A square RGBA8 image with the full chain and a 128-texel tail.
StreamedImage Square(uint32_t size)
{
    StreamedImage image;
    image.width = image.height = size;
    image.levels = 1;
    for (uint32_t s = size; s > 1; s /= 2) ++image.levels;
    image.texelBytes = 4;
    image.tailLevel = TailLevel(size, size, image.levels, 128);
    return image;
}

// One feedback round: image i asked for level finest[i] (kNoRequest: none)
// with weight counts[i] (1 without counts).
bool Feed(StreamingPlanner& planner, std::vector<uint32_t> const& finest, std::vector<uint32_t> const& counts = {})
{
    std::vector<uint32_t> bins(finest.size() * kFeedbackLevels, 0);
    for (size_t i = 0; i < finest.size(); ++i)
        if (finest[i] != kNoRequest) bins[i * kFeedbackLevels + finest[i]] = counts.empty() ? 1u : counts[i];
    return planner.addFeedback(bins.data(), finest.size());
}

std::string Levels(std::vector<uint32_t> const& plan)
{
    std::string text;
    for (uint32_t l : plan) text += std::to_string(l) + ' ';
    return text;
}

} // namespace

int main()
{
    // Tails and level sizes.
    check(TailLevel(4096, 4096, 13, 128) == 5, "tail of a 4K image");
    check(TailLevel(4096, 1024, 13, 128) == 5, "tail of a wide image (larger side)");
    check(TailLevel(100, 50, 7, 128) == 0, "small images are their own tail");
    const StreamedImage k1 = Square(1024); // levels 11, tail 3 (128)
    check(k1.levels == 11 && k1.tailLevel == 3, "1K image levels and tail");
    check(StreamedLevelBytes(k1, 0) == 1024ull * 1024 * 4 && StreamedLevelBytes(k1, 10) == 4, "level bytes");
    check(StreamedBytes(k1, 3) == StreamedBytes(k1, 4) + StreamedLevelBytes(k1, 3), "range bytes");

    // Without requests only tails are planned; a request coarser than the
    // tail asks for nothing.
    {
        StreamingPlanner planner;
        planner.reset({k1, k1}, uint64_t(1) << 30);
        check(planner.plan() == std::vector<uint32_t>({3, 3}), "no requests: tails");
        check(!Feed(planner, {5, kNoRequest}), "a request coarser than the tail is not finer");
        check(planner.plan() == std::vector<uint32_t>({3, 3}), "coarse request: tails");
        check(planner.stats(planner.plan()).requested == 0, "coarse requests are not streamed");
    }

    // Enough budget: every request is planned; levels are planned down to
    // the request only.
    {
        StreamingPlanner planner;
        planner.reset({k1, k1, Square(64)}, uint64_t(1) << 30);
        check(Feed(planner, {0, 2, 0}), "finer requests");
        const auto plan = planner.plan();
        check(plan == std::vector<uint32_t>({0, 2, 0}), "ample budget plans the requests: " + Levels(plan));
        const auto stats = planner.stats(plan);
        check(stats.requested == 2 && stats.belowRequested == 0 && stats.pending == 2, "ample budget stats");
    }

    // Tight budget: tails first, then levels by request count per byte.
    // Image 0 is asked for 100 times, image 1 once; the budget holds the
    // tails, image 0 down to level 1 and image 1 down to level 2.
    {
        const uint64_t tails = 2 * StreamedBytes(k1, 3);
        const uint64_t budget = tails + (StreamedBytes(k1, 1) - StreamedBytes(k1, 3)) +
            (StreamedBytes(k1, 2) - StreamedBytes(k1, 3)) + 1000;
        StreamingPlanner planner;
        planner.reset({k1, k1}, budget);
        Feed(planner, {0, 0}, {100, 1});
        const auto plan = planner.plan();
        check(plan == std::vector<uint32_t>({1, 2}), "priority by count per byte: " + Levels(plan));
        const auto stats = planner.stats(plan);
        check(stats.belowRequested == 2 && stats.plannedBytes <= budget, "tight budget stats");
        // Same feedback, same plan.
        StreamingPlanner again;
        again.reset({k1, k1}, budget);
        Feed(again, {0, 0}, {100, 1});
        check(again.plan() == plan, "plans are deterministic");
        // A budget below the tails keeps the tails.
        StreamingPlanner tiny;
        tiny.reset({k1, k1}, 1000);
        Feed(tiny, {0, 0}, {100, 1});
        check(tiny.plan() == std::vector<uint32_t>({3, 3}), "tails always stay");
    }

    // Weighted requests: levels asked for by at most `threshold` of an image's
    // lookup weight are left out; with threshold 0 every lookup counts.
    {
        StreamingPlanner planner;
        planner.reset({k1}, uint64_t(1) << 30, {}, 0.01);
        std::vector<uint32_t> bins(kFeedbackLevels, 0);
        bins[0] = 5;     // 0.5% of the weight asks for level 0
        bins[2] = 995;
        planner.addFeedback(bins.data(), 1);
        check(planner.request(0) == 2, "a negligible fine request is left out");
        bins[0] = 20;    // now 1.5% overall: level 0 is needed
        bins[2] = 0;
        check(planner.addFeedback(bins.data(), 1) && planner.request(0) == 0, "a significant fine request counts");
        StreamingPlanner exact;
        exact.reset({k1}, uint64_t(1) << 30, {}, 0.0);
        bins = std::vector<uint32_t>(kFeedbackLevels, 0);
        bins[1] = 1;
        bins[4] = 100000;
        exact.addFeedback(bins.data(), 1);
        check(exact.request(0) == 1, "threshold 0 keeps the finest request");
    }

    // Levels resident and requested in this accumulation are never demoted
    // for a newer request, even a more frequent one.
    {
        const uint64_t budget = 2 * StreamedBytes(k1, 3) + (StreamedBytes(k1, 1) - StreamedBytes(k1, 3)) + 1000;
        StreamingPlanner planner;
        planner.reset({k1, k1}, budget);
        Feed(planner, {1, kNoRequest});
        check(planner.plan() == std::vector<uint32_t>({1, 3}), "first request planned");
        planner.setResident(0, 1);
        Feed(planner, {kNoRequest, 0}, {0, 1000});
        const auto plan = planner.plan();
        check(plan[0] == 1, "a converging image keeps its levels: " + Levels(plan));
        check(plan[1] == 3, "the newer request waits for budget: " + Levels(plan));
        check(planner.stats(plan).belowRequested == 1, "newer request below requested");
        // Planned from the requests alone, residency does not matter.
        StreamingPlanner other;
        other.reset({k1, k1}, budget);
        Feed(other, {1, kNoRequest});
        Feed(other, {kNoRequest, 0}, {0, 1000});
        check(other.plan(true) == planner.plan(true), "request-only plans ignore residency: " +
                                                          Levels(other.plan(true)) + "/ " + Levels(planner.plan(true)));
    }

    // A new accumulation: earlier levels stay while the budget allows, and
    // the least recently requested are evicted first.
    {
        const StreamedImage k2 = Square(512); // levels 10, tail 2
        const uint64_t fineA = StreamedBytes(k2, 0) - StreamedBytes(k2, 2);
        const uint64_t budget = 3 * StreamedBytes(k2, 2) + 2 * fineA + 1000;
        StreamingPlanner planner;
        planner.reset({k2, k2, k2}, budget);
        Feed(planner, {0, kNoRequest, kNoRequest}); // image 0, round 1
        Feed(planner, {kNoRequest, 0, kNoRequest}); // image 1, round 2
        planner.setResident(0, 0);
        planner.setResident(1, 0);
        check(planner.plan() == std::vector<uint32_t>({0, 0, 2}), "both resident");
        planner.beginAccumulation();
        check(planner.plan() == std::vector<uint32_t>({0, 0, 2}), "a new view keeps what fits");
        Feed(planner, {kNoRequest, kNoRequest, 0}); // image 2 needs one image's levels
        const auto plan = planner.plan();
        check(plan == std::vector<uint32_t>({2, 0, 0}), "least recently requested evicted: " + Levels(plan));
        // Partial eviction: levels go finest first while coarser ones fit.
        StreamingPlanner partial;
        partial.reset({k2, k2}, 2 * StreamedBytes(k2, 2) + 2 * StreamedLevelBytes(k2, 1) + 100);
        Feed(partial, {0, kNoRequest});
        partial.setResident(0, 0);
        partial.beginAccumulation();
        Feed(partial, {kNoRequest, 1});
        const auto kept = partial.plan();
        check(kept[1] == 1 && kept[0] == 1, "eviction keeps the coarser levels that fit: " + Levels(kept));
    }

    if (failures) {
        std::cerr << failures << " texture streaming check(s) failed\n";
        return 1;
    }
    std::cout << "texture streaming planner checks passed\n";
    return 0;
}
