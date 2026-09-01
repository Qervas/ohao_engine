// Stage 2 Task 1, check 50: the adjoint seed IS dL/d(film).
//
// Every Stage 1 gradient is dJ/dtheta for J = the sum of every float in the
// film, which makes dJ/d(film[p][c]) identically 1. That is not a
// configuration choice: it is why the backward pass could seed its adjoint
// with the path's own throughput and carry no separate quantity. A real loss
// supplies a different dL/dpixel per pixel and per channel, and this check is
// what says the shader reads it.
//
// THE ORACLE IS A PARTITION IDENTITY, NOT A FINITE DIFFERENCE. dL/dtheta is
// linear in the seed:
//
//     G(w) = SUM_p SUM_c w[p][c] * dI_{p,c}/dtheta
//
// so for any split of the pixels into two disjoint sets L and R that together
// cover the film,
//
//     G(1_L) + G(1_R) = G(1)
//
// exactly -- no step size, no truncation term, no tolerance beyond the
// arena's own atomic floor. It needs no second oracle because it compares
// three runs of the SAME machinery against an algebraic identity that holds
// whatever the gradient's value is; a wrong gradient that is linear in the
// seed still satisfies it, which is precisely why the identity tests the
// SEED and not the gradient. Checks 37-49 already test the gradient.
//
// WHY IT FAILS BEFORE THE IMPLEMENTATION EXISTS. A shader that ignores the
// seed returns G(1) for every w, so the left-hand side is 2*G(1) and the
// halves each equal the whole. Both the identity and the non-vacuity
// assertion below reject that, which is what makes this a check written
// first rather than a check written to pass.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

/// Check 50: dL/dtheta is linear in the seed, and the seed selects pixels.
bool checkAdjointSeed(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
