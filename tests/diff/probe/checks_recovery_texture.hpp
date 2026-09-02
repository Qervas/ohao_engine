// Stage 2 Task 5 Step 3, check 55: Gate 5 for many parameters at once.
//
// Check 54 recovers a SCALAR. This recovers a 4x3x3 emission texture element
// by element: the multi-parameter half of Gate 5, and the only check that
// runs the full optimisation loop over many parameters at once.
//
// WHAT IT ADDS, STATED NARROWLY BECAUSE THE OBVIOUS CLAIM DID NOT SURVIVE
// TESTING. An earlier version of this comment said it would catch a scatter
// transposition that checks 44/45/49 have a shared blind spot for. Two
// attempts to construct such a bug were both caught EARLIER: rotating the
// channel index in diffTexelElementIndex is rejected by
// checkTexelOrderingTie before any GPU work, and swapping the bilinear
// weights w10/w01 in the scatter is rejected by check 45. That is evidence
// against the claim, not for it, so the claim is withdrawn rather than left
// standing.
//
// What it does add is real and smaller: it is the only check that closes the
// LOOP over many parameters -- forward, loss, backward, step, repeated -- and
// the only one that asserts the unconstrained elements are EXACTLY unmoved.
//
// NOT EVERY TEXEL IS RECOVERABLE, and that is a property of the scene rather
// than a weakness of the gate: only texels the paths actually read carry
// gradient. The unconstrained ones are not "unrecovered"; demanding they
// arrive would be demanding the impossible. Which is which is read off the
// FIRST iteration's gradient, before any optimisation, so the partition
// cannot be chosen from the result.
//
// The unconstrained elements carry their own assertion: they must be EXACTLY
// unmoved. Adam at g = 0 has m = v = 0 and a step of alpha*0/(0+eps), which
// is zero and not merely small, so an element that drifted is the optimiser
// writing a parameter the scene does not depend on.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkRecoveryTexture(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
