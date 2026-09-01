// The stage gate, checks 33-34: the whole wavefront integrator against an
// INDEPENDENT CPU reference path tracer, per pixel and pooled.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkIntegratorParity(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
