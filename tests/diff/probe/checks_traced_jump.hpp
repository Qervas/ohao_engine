// Item 6 task 2, check 69: THE RADIANCE, TRACED.
//
// The last Stage 3 deviation said the boundary term's two radiances are
// "pushed, not traced". Check 64 closed the half that mattered most -- the
// jump now VARIES along an edge. This closes the other half: the jump is read
// from the SCENE.
//
// THE CONTROL IS INVERTED relative to check 64's, and the inversion is the
// whole design. There, the pushed form had to FAIL, because the claim was
// that a varying jump is necessary. Here the pushed form must AGREE, because
// the claim is that tracing FINDS what a human previously typed in. On a
// scene with one emissive surface against a background, `lIn` and `lOut` are
// exactly the right two numbers -- so a disagreement means the trace is
// wrong, not that the layer is needed.
//
// WHAT IT WOULD CATCH. The screen-to-world map is affine and the caller
// supplies it; get it wrong and the rays sample the right scene at the wrong
// place, which produces a plausible gradient rather than an error. Sample on
// the wrong side of the edge and the jump comes back negated, which is
// gradient ascent -- the failure that has now bitten this subsystem three
// times through the winding convention. Both land here as a disagreement
// with numbers that are known exactly.
//
// THE INTERIOR TERM IS STILL EXACTLY ZERO, which is what keeps this
// attributable: emission is a constant per surface, so it does not depend on
// where the vertices are. Tracing does not change that -- it changes only
// where the number comes from.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkTracedJump(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
