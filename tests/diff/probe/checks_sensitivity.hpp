// Check 73: THE SENSITIVITY MAP (spec 10.2) -- dpixel/dtheta as an image.
//
// WHAT IT IS. The replay hook already computes one scalar per hit vertex --
// that vertex's contribution to dL/dtheta -- and sums every one of them into
// a single arena slot. A sensitivity map is the SAME scalars binned by the
// pixel each vertex belongs to instead of summed. So the map answers "where
// does the loss's dependence on theta come from" with the numbers that
// already answer "how much".
//
// WHICH IS WHY THIS CHECK NEEDS NO ORACLE OF ITS OWN. The gate is an
// IDENTITY: the sum over the map must equal the arena's scalar, term for
// term, differing only by the order two atomicAdds accumulated the same
// numbers. That is worth more than any tolerance against a hand-derived
// expectation, because it makes the map inherit every check that already
// gates the gradient -- check 37's finite difference above all, which is the
// one that establishes the scalar is the derivative of the film at all.
//
// AND THE NULL TEST, which is what the identity cannot see. A map that was
// uniformly wrong -- every cell holding the same 1/N of the total -- would
// satisfy the sum exactly. So the scene is built so that a KNOWN SET OF
// PIXELS cannot depend on theta: their primary rays miss the geometry
// entirely, so no vertex ever lands in them. Those cells are required to be
// EXACTLY 0.0f, compared as floats and not through a tolerance, in the
// manner of check 59's null test.
//
// THE SET IS DERIVED ON PAPER, from the camera basis and the plate's edge,
// before the render -- not read off the map. A miss set inferred from the
// zeros in the map is the defect class this subsystem has now hit three
// times: a check whose expected value comes from the same source as the
// measured one cannot fail.
//
// NO ADJOINT SEED, deliberately. The hook multiplies its contribution by
// dL/dpixel before scattering, so with a seed bound the map holds
// seed[p] * d(film[p])/d(theta) -- a per-pixel decomposition of the LOSS
// gradient, which is a useful picture but is not the sensitivity image spec
// 10.2 asks for. Unbound, diffAdjointSeed returns 1 and the map is
// d(film[p])/d(theta) exactly.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkSensitivityMap(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
