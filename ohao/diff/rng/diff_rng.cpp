#include "diff/rng/diff_rng.hpp"

namespace ohao::diff {
namespace {

// PCG hash. Mirrored verbatim in shaders/includes/diff/rng.glsl.
std::uint32_t pcgHash(std::uint32_t input) noexcept {
    std::uint32_t state = input * 747796405u + 2891336453u;
    std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

}  // namespace

PathRng PathRng::forPath(std::uint32_t pixelIndex, std::uint32_t sampleIndex,
                         std::uint32_t iterationSeed) noexcept {
    PathRng rng;
    // Hash each component in turn so no two tuples collide cheaply.
    rng.m_state = pcgHash(pixelIndex ^ pcgHash(sampleIndex ^ pcgHash(iterationSeed)));
    rng.m_draws = 0;
    return rng;
}

float PathRng::next1D() noexcept {
    m_state = pcgHash(m_state);
    ++m_draws;
    // 24 mantissa bits -> [0, 1). Dividing by 2^24 cannot round up to 1.0f.
    return static_cast<float>(m_state >> 8u) * (1.0f / 16777216.0f);
}

}  // namespace ohao::diff
