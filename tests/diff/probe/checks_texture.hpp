// The texture scatter, checks 44-45: the bilinear weights conserve the
// incoming adjoint and land only in the host-predicted footprint, and the
// per-element magnitude gate.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkTextureScatter(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
