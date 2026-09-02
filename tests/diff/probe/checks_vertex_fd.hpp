// Stage 3 Task 4, check 56: the vertex-position finite difference.
//
// This is the target the boundary term will have to reproduce, established
// BEFORE the machinery that produces it -- and the measurement that says why
// that machinery is needed at all.
//
// Every parameter Stages 1-2 differentiate leaves the PATHS untouched under
// a +/-h perturbation; checks 37 and 42 assert exactly 0 trace mismatches,
// and that is what makes their finite difference the derivative of ONE
// realisation of the estimator rather than a difference of two Monte Carlo
// means. A VERTEX is the first parameter for which that is false by
// construction: moving geometry moves every ray that hits it. The difference
// is then taken across a DISCONTINUITY, which is precisely what spec 4.1's
// boundary term accounts for.
//
// The nonzero mismatch count also confirms something the probe would
// otherwise only assume: the acceleration structure really is rebuilt from
// the perturbed positions on every call, so the per-iteration BLAS refit
// hazard is already satisfied here by construction.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkVertexFiniteDifference(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
