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

std::vector<uint32_t> Requests(size_t n, uint32_t level)
{
    return std::vector<uint32_t>(n, level);
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
        const uint32_t finest[] = {5, kNoRequest};
        check(!planner.addFeedback(finest, nullptr, 2), "a request coarser than the tail is not finer");
        check(planner.plan() == std::vector<uint32_t>({3, 3}), "coarse request: tails");
        check(planner.stats(planner.plan()).requested == 0, "coarse requests are not streamed");
    }

    // Enough budget: every request is planned; levels are planned down to
    // the request only.
    {
        StreamingPlanner planner;
        planner.reset({k1, k1, Square(64)}, uint64_t(1) << 30);
        const uint32_t finest[] = {0, 2, 0};
        check(planner.addFeedback(finest, nullptr, 3), "finer requests");
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
        const uint32_t finest[] = {0, 0};
        const uint32_t counts[] = {100, 1};
        planner.addFeedback(finest, counts, 2);
        const auto plan = planner.plan();
        check(plan == std::vector<uint32_t>({1, 2}), "priority by count per byte: " + Levels(plan));
        const auto stats = planner.stats(plan);
        check(stats.belowRequested == 2 && stats.plannedBytes <= budget, "tight budget stats");
        // Same feedback, same plan.
        StreamingPlanner again;
        again.reset({k1, k1}, budget);
        again.addFeedback(finest, counts, 2);
        check(again.plan() == plan, "plans are deterministic");
        // A budget below the tails keeps the tails.
        StreamingPlanner tiny;
        tiny.reset({k1, k1}, 1000);
        tiny.addFeedback(finest, counts, 2);
        check(tiny.plan() == std::vector<uint32_t>({3, 3}), "tails always stay");
    }

    // Levels resident and requested in this accumulation are never demoted
    // for a newer request, even a more frequent one.
    {
        const uint64_t budget = 2 * StreamedBytes(k1, 3) + (StreamedBytes(k1, 1) - StreamedBytes(k1, 3)) + 1000;
        StreamingPlanner planner;
        planner.reset({k1, k1}, budget);
        const uint32_t first[] = {1, kNoRequest};
        planner.addFeedback(first, nullptr, 2);
        check(planner.plan() == std::vector<uint32_t>({1, 3}), "first request planned");
        planner.setResident(0, 1);
        const uint32_t second[] = {kNoRequest, 0};
        const uint32_t counts[] = {0, 1000};
        planner.addFeedback(second, counts, 2);
        const auto plan = planner.plan();
        check(plan[0] == 1, "a converging image keeps its levels: " + Levels(plan));
        check(plan[1] == 3, "the newer request waits for budget: " + Levels(plan));
        check(planner.stats(plan).belowRequested == 1, "newer request below requested");
    }

    // A new accumulation: earlier levels stay while the budget allows, and
    // the least recently requested are evicted first.
    {
        const StreamedImage k2 = Square(512); // levels 10, tail 2
        const uint64_t fineA = StreamedBytes(k2, 0) - StreamedBytes(k2, 2);
        const uint64_t budget = 3 * StreamedBytes(k2, 2) + 2 * fineA + 1000;
        StreamingPlanner planner;
        planner.reset({k2, k2, k2}, budget);
        const uint32_t a[] = {0, kNoRequest, kNoRequest};
        planner.addFeedback(a, nullptr, 3); // image 0, round 1
        const uint32_t b[] = {kNoRequest, 0, kNoRequest};
        planner.addFeedback(b, nullptr, 3); // image 1, round 2
        planner.setResident(0, 0);
        planner.setResident(1, 0);
        check(planner.plan() == std::vector<uint32_t>({0, 0, 2}), "both resident");
        planner.beginAccumulation();
        check(planner.plan() == std::vector<uint32_t>({0, 0, 2}), "a new view keeps what fits");
        const uint32_t c[] = {kNoRequest, kNoRequest, 0};
        planner.addFeedback(c, nullptr, 3); // image 2 needs one image's levels
        const auto plan = planner.plan();
        check(plan == std::vector<uint32_t>({2, 0, 0}), "least recently requested evicted: " + Levels(plan));
        // Partial eviction: levels go finest first while coarser ones fit.
        StreamingPlanner partial;
        partial.reset({k2, k2}, 2 * StreamedBytes(k2, 2) + 2 * StreamedLevelBytes(k2, 1) + 100);
        const uint32_t both[] = {0, kNoRequest};
        partial.addFeedback(both, nullptr, 2);
        partial.setResident(0, 0);
        partial.beginAccumulation();
        const uint32_t other[] = {kNoRequest, 1};
        partial.addFeedback(other, nullptr, 2);
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
