// Stage 2 Task 4, check 53: a multi-view batch is the sum of its views.
//
// dL/dtheta is a SUM over paths and the arena is an atomicAdd accumulator, so
// accumulating two views into one arena and adding two separately-accumulated
// arenas are the same arithmetic in a different order. Only the float32
// atomic order distinguishes them.
//
// The check also asserts, separately, that the batch does NOT equal its last
// view. That is what a clear running per VIEW rather than per ITERATION
// (spec 4.4) produces, and the resulting gradient is entirely plausible --
// it is a real gradient, of one view instead of two -- so nothing else in
// the probe would notice.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkMultiViewBatch(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
