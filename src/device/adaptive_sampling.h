#pragma once

// Adaptive sampling: a pixel stops receiving samples once its estimated error
// falls below a threshold. The error is the two-buffer estimate also used by
// Cycles: the difference between the mean of all samples and the mean of the
// even-numbered ones, relative to the square root of the mean. A pixel stays
// active while it or a neighbor in its 3x3 window is not converged, and a
// converged pixel never becomes active again, so every active pixel has the
// same sample count (and sampler index).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace prism::device {

struct AdaptiveSettings {
    float threshold = 0.0f; // 0 disables adaptive sampling
    int minSamples = 16;    // samples before the first convergence check
    int step = 8;           // samples between checks

    bool Enabled() const { return threshold > 0.0f; }
    // Whether to check convergence once `samples` samples are accumulated.
    bool CheckAfter(int samples) const
    {
        return Enabled() && samples >= minSamples && (samples - minSamples) % step == 0;
    }
};

// Error of a pixel from the RGB sums of all its samples and of its
// even-numbered samples (0, 2, 4, ...), after `samples` samples.
inline float AdaptiveError(const float sum[3], const float evenSum[3], int samples)
{
    const float inverse = 1.0f / float(samples);
    const float halfInverse = 1.0f / float((samples + 1) / 2);
    float difference = 0.0f, mean = 0.0f;
    for (int c = 0; c < 3; ++c) {
        difference += std::abs(sum[c] * inverse - evenSum[c] * halfInverse);
        mean += sum[c] * inverse;
    }
    return difference / (0.0001f + std::sqrt(std::max(0.0f, mean)));
}

// Per-pixel active flags of a width x height image, top row first.
class ActivePixels {
public:
    void Reset(unsigned width, unsigned height)
    {
        m_width = width;
        m_height = height;
        m_active.assign(size_t(width) * height, 1);
        m_count = m_active.size();
    }

    bool Active(size_t pixel) const { return m_active.empty() || m_active[pixel] != 0; }
    size_t Count() const { return m_count; }
    uint8_t const* Mask() const { return m_active.data(); }

    // Deactivates converged pixels; error(pixel) is evaluated for active
    // pixels only. Returns the number of pixels still active.
    template <typename Error>
    size_t Update(float threshold, Error&& error)
    {
        std::vector<uint8_t> unconverged(m_active.size(), 0);
        for (size_t p = 0; p < m_active.size(); ++p)
            unconverged[p] = m_active[p] && !(error(p) < threshold);
        m_count = 0;
        for (unsigned y = 0; y < m_height; ++y)
            for (unsigned x = 0; x < m_width; ++x) {
                const size_t p = size_t(y) * m_width + x;
                if (!m_active[p]) continue;
                bool keep = false;
                for (unsigned ny = y > 0 ? y - 1 : 0; !keep && ny <= std::min(y + 1, m_height - 1); ++ny)
                    for (unsigned nx = x > 0 ? x - 1 : 0; !keep && nx <= std::min(x + 1, m_width - 1); ++nx)
                        keep = unconverged[size_t(ny) * m_width + nx] != 0;
                m_active[p] = keep;
                m_count += keep;
            }
        return m_count;
    }

private:
    unsigned m_width = 0, m_height = 0;
    std::vector<uint8_t> m_active;
    size_t m_count = 0;
};

} // namespace prism::device
