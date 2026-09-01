// Environment checks 24-28: importance sampling against an independent
// oracle, the pdf identities, the production push-constant fill, and the
// shadow ray.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkEnvImportanceSampling(ohao::diff::GpuProbeContext& ctx);

bool checkEnvPushFillAndShadowRay(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
