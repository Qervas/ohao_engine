// The shared scene: one geometry, one environment, one camera.
//
// Checks 33-34 (the integrator parity gate) and the Stage 1 gradient checks
// run against the SAME configuration, built here once rather than
// transcribed into each. scene.cpp states, piece by piece, why every quad is
// where it is and why the environment is built the way it is -- that
// reasoning is what makes the two gates comparable, so it travels with the
// code.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstdint>
#include <vector>

namespace ohao::diff::probe {

void buildParityScene(std::vector<float>& positions, std::vector<uint32_t>& indices);

void buildParityEnvironment(uint32_t envW, uint32_t envH, std::vector<float>& outRgba,
                            std::vector<double>& outLum);

ohao::diff::WavefrontGenerateCamera parityCamera();

}  // namespace ohao::diff::probe
