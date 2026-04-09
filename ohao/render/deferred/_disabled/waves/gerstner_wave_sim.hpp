#pragma once

#include "i_wave_sim.hpp"

namespace ohao {

// Stub implementation — the Gerstner variant computes waves entirely in
// water.vert (inline shader math), so no GPU resources are needed here.
// WaterPass uses GerstnerWaveSim as the default when no FFT sim is set.
class GerstnerWaveSim final : public IWaveSim {
public:
    // No GPU work: Gerstner math is inline in water.vert.
    bool providesTextures() const override { return false; }
};

} // namespace ohao
