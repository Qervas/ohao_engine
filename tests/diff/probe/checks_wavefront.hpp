// Wavefront checks 7-19: the path-state buffers, and the generate,
// intersect and scatter stages, run both stage-by-stage and fused.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkWavefrontBuffersZero(ohao::diff::GpuProbeContext& ctx);

bool checkWavefrontGenerate(ohao::diff::GpuProbeContext& ctx);

bool checkPathStateLayoutMapping(ohao::diff::GpuProbeContext& ctx);

bool checkWavefrontIntersect(ohao::diff::GpuProbeContext& ctx);

bool checkEmptyIndirectDispatch(ohao::diff::GpuProbeContext& ctx);

bool checkWavefrontScatter(ohao::diff::GpuProbeContext& ctx);

bool checkFusedBounceLoop(ohao::diff::GpuProbeContext& ctx);

bool checkGeometricNormals(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
