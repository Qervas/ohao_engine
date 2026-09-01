// Replay equivalence, checks 35-36: the forward vertex trace is real and
// independently correct, and the replay instantiation walks it bit for bit.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkReplayEquivalence(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
