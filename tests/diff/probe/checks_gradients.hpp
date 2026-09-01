// The gradients, checks 37-43: dJ/d(albedo) and its null test, the GGX
// roughness/metallic adjoints with the detached-sampling bias measured,
// and dJ/d(emission) with its own null test.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkAlbedoGradient(ohao::diff::GpuProbeContext& ctx);

bool checkGgxGradients(ohao::diff::GpuProbeContext& ctx);

bool checkEmissionGradient(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
