// Stage 3, check 59: the silhouette pass's flags driving the boundary pass.
//
// THE NULL TEST FOR THE BOUNDARY TERM. A vertex lying on no silhouette edge
// receives EXACTLY 0.0f -- compared as a float, not through a tolerance --
// because the scatter only ever writes the two endpoints of an edge it
// processed. That is the geometric analogue of checks 38/43/47, and it is
// the one assertion a wrong filter cannot survive: an unfiltered pass writes
// every vertex of every edge.
//
// FILTERING IS CORRECTNESS, NOT SPEED. An interior edge has the same surface
// on both sides, so its radiance jump is zero and the boundary integrand
// vanishes there; evaluating it anyway with a pushed jump invents a
// discontinuity the geometry does not have. The check asserts that directly,
// by running both ways and requiring the results to differ.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkBoundaryOverSilhouette(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
