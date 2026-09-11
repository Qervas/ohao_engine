// Item 6 task 3, check 70: THE ORDER OF THE QUADRATURE.
//
// Check 69 traces a radiance that is constant per surface, so the jump along
// a chord is piecewise constant and the moment integral is EXACT. There is no
// truncation term there, which is why it takes a fixed tolerance -- and why
// the roadmap's blanket claim that item 6 wants a convergence gate was only
// half right.
//
// This is the other half. The radiance now varies WITHIN a surface, so the
// midpoint sample per sub-chord really is a quadrature, and a quadrature is
// only trustworthy if its error falls at the rate the derivation predicts.
//
// THE PREDICTION, stated before measuring. Sampling the jump at a sub-chord's
// midpoint while integrating the vertex weight exactly leaves
//
//     INTEGRAL (jump(u) - jump(mid)) * weight(u) du
//
// over each sub-interval. Expanding jump about the midpoint, the FIRST-order
// term does not vanish -- weight(u) is not symmetric about mid, and
// INTEGRAL t*(1-mid-t) dt = -h^3/12 -- so each sub-interval contributes
// O(h^3) and the N = L/h of them sum to O(h^2). **Doubling the subdivisions
// should quarter the error: a ratio of 4.**
//
// MEASURED TWICE, because the two questions are different. The ORDER is
// measured against a heavily subdivided run of the same kernel, which is
// clean but self-referential -- it shows the quadrature converges, not that
// it converges to the right number. So that heavily subdivided run is
// separately checked against a SUPERSAMPLED IMAGE DERIVATIVE, which shares
// no line with it. Order from one, correctness from the other.
//
// The radiance is a SINE and not a polynomial on purpose: an affine
// variation is integrated exactly by the moments (check 64), so it would
// leave nothing whose order could be measured.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkShadedOrder(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
