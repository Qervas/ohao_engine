// BSDF checks 20-23: the lobe itself against the CPU oracle, then the
// three furnace tests at the three values of the lobe probability.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkBsdfTerms(ohao::diff::GpuProbeContext& ctx);

bool checkFurnaces(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
