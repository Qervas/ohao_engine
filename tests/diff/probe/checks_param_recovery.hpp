// Stage 3, check 62: RECOVERY THROUGH THE PARAMETERISATION.
//
// Spec 9 asks Stage 3 to parameterise "something that produces vertex
// positions, NEVER vertex positions directly". Check 60 optimises the six
// position components themselves -- the trivial case, and the one the
// Stage 3 plan's own Global Constraint rules out. `AffineVertexParameterisation`
// closed that as CODE; this closes it as a GATE: three parameters
// (tx, ty, log-scale) recovered from a synthetic target, with the boundary
// pass's dL/d(position) reaching them only through the pullback.
//
// WHAT THIS GATE CAN AND CANNOT SEE. The target is exactly reachable, so at
// the optimum every position gradient vanishes and therefore so does EVERY
// pullback of it, right or wrong -- the optimum is a common fixed point.
// That makes a pure magnitude error in the Jacobian invisible here, and
// Adam makes it doubly so: it divides by sqrt(v), so any positive rescaling
// of a gradient component changes nothing at all. Magnitude is gated
// elsewhere, by the unit test that builds J column by column from finite
// differences of `apply`. What this gate sees is the part that test cannot
// reach: that the three-parameter descent, run end to end through the GPU
// boundary pass, actually arrives -- and it is made non-vacuous by a
// CONTROL RUN with the scale column's sign flipped, which must not.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkParameterisedRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
