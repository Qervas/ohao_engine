// Stage 2 Task 3, check 52: Adam over a parameter block.
//
// Two oracles, and the first is what makes the second non-circular.
//
// THE FIRST STEP IN CLOSED FORM, derived on paper. With m_0 = v_0 = 0,
// Kingma & Ba's bias correction makes mHat_1 = g and vHat_1 = g^2 EXACTLY,
// so theta_1 = theta_0 - alpha * g / (|g| + eps) -- alpha * sign(g), for any
// gradient. No reference implementation is involved.
//
// THE WHOLE TRAJECTORY against a CPU Adam written from the PAPER, not from
// the shader. Fifty steps, compared at every one.
//
// WHY THE TRAJECTORY AND NOT THE ENDPOINT. Dropping the bias correction
// makes the first step 3.16x too large and is within a percent of correct by
// t ~ 100, so an uncorrected Adam still converges on a well-conditioned
// objective. An endpoint check would pass on it.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkAdam(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
