// The forward model the two geometry gates descend (checks 60 and 62).
//
// A COVERAGE IMAGE, SUPERSAMPLED ON THE HOST rather than traced. That is
// what makes those gates measurable: with constant radiance inside and
// outside the triangle, moving a vertex changes NOTHING but coverage, so
// spec 4.1's interior integral is exactly zero and the derivative being
// descended is purely the boundary term. Against a shaded scene the two are
// summed and a failed recovery could not be attributed to either.
//
// Shared by both gates deliberately. Each builds its target with the same
// function it renders through, so a systematic error in this model cancels
// -- which is sound here only because what those gates test is the LOOP
// (seed, pullback, step, order), not the renderer. The boundary term itself
// is gated against an independent supersampled image derivative in check 57.
#pragma once

#include <cstdint>
#include <vector>

namespace ohao::diff::probe {

/// Point-in-triangle by three cross products, accepting either winding.
/// `tri` is 2 floats per vertex, three vertices.
[[nodiscard]] bool coverageInsideTriangle(const std::vector<float>& tri, double px, double py);

/// One float per pixel: `lIn` where covered, `lOut` where not, linearly
/// blended by the supersampled coverage fraction (`sub` x `sub` per pixel).
[[nodiscard]] std::vector<float> renderTriangleCoverage(const std::vector<float>& tri,
                                                        std::uint32_t image, std::uint32_t sub,
                                                        double lIn, double lOut);

}  // namespace ohao::diff::probe
