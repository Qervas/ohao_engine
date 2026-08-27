#pragma once

#include <cstdint>

namespace ohao::diff {

/// PCG-based path RNG. A path is a pure function of (pixel, sample, seed).
///
/// The forward and backward kernels reconstruct this from the tuple alone --
/// no state crosses the kernel boundary. If the two ever consume a different
/// number of draws, the replayed path is a DIFFERENT path and every gradient
/// is silently wrong, which is why drawCount() exists.
///
/// shaders/includes/diff/rng.glsl mirrors this math exactly. Change both or
/// neither.
class PathRng {
public:
    [[nodiscard]] static PathRng forPath(std::uint32_t pixelIndex,
                                         std::uint32_t sampleIndex,
                                         std::uint32_t iterationSeed) noexcept;

    /// Next uniform value in [0, 1).
    [[nodiscard]] float next1D() noexcept;

    [[nodiscard]] std::uint32_t drawCount() const noexcept { return m_draws; }

private:
    std::uint32_t m_state{0};
    std::uint32_t m_draws{0};
};

}  // namespace ohao::diff
