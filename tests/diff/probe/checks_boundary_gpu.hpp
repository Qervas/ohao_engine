// Stage 3, check 57: the boundary kernel on the GPU.
//
// Checked twice, and the second is what matters. Against the HOST form,
// which diff_unit_tests already gates against a supersampled derivative --
// so a disagreement localises to the dispatch (the clip, the normal's side,
// the barycentric weights, the arena addressing) rather than to the
// mathematics. And against the SUPERSAMPLED ORACLE directly, so the GPU is
// tied to something that shares no line with it.
//
// That oracle counts points inside a triangle and differences the result. It
// has no edge, no chord, no normal and no weight, which is why it could see
// this term's sign error and then a 3x orientation error when neither the
// closed form nor the sampled estimator could -- they shared the mistakes.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkBoundaryGpu(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
