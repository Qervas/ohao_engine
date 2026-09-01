// Stage 2 Task 2, check 51: the L2 loss kernel.
//
// The gate is a finite difference ON THE LOSS ALONE -- no renderer, no path,
// no arena -- so a failure is unambiguously this kernel's. That is the whole
// reason the loss is a separate task and a separate shader.
//
// The difference is EXACT rather than approximate: L is a quadratic in the
// film and a central difference is exact through degree 2, so its truncation
// error is identically zero and there is no step size to derive. Stage 1
// Task 6 established the same property for the albedo at one and two bounces.
//
// One assertion is derived on paper rather than from another of the kernel's
// own outputs, and it is what pins N. film = 0 against target = c gives
// L = c^2 for any N; a mean over PIXELS rather than floats would return three
// times that, and nothing downstream would notice -- Gate 5 would absorb the
// factor into the learning rate and converge anyway.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkLossL2(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
