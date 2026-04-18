#pragma once

// DenoiseMode — selects the denoiser backend used by the offline path tracer.
//
// Sub-plan 1 ships OIDN. Later sub-plans add OptiX, NRD, DLSS RR.
// The enum + parse helpers live here so every backend can share them and
// the CLI surface stays consistent across examples.

#include <cstdint>
#include <string>

namespace ohao {

enum class DenoiseMode : uint32_t {
    None   = 0,
    OIDN   = 1,
    OptiX  = 2,   // NVIDIA OptiX (requires CUDA + OptiX SDK at build time)
    // future:
    // NRD    = 3,
    // DLSSRR = 4,
};

// Parse a CLI string (case-insensitive). Unknown values return None and
// log a warning to stderr.
DenoiseMode parseDenoiseMode(const std::string& s);

// Human-readable lowercase name. Stable, safe for CLI round-trip.
const char* denoiseModeName(DenoiseMode mode);

} // namespace ohao
