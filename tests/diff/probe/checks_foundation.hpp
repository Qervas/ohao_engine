// Foundation checks 1-6: the arena, the pipeline and stage lifecycles,
// the ray query, the registry/arena seam, and CPU/GPU RNG parity.
//
// Lifted verbatim out of diff_gpu_probe.cpp's main(): each function is one of
// main()'s former top-level braced scopes, returning false where it used to
// `return 1`. Nothing about what any check compares changed.
#pragma once

#include "gpu_probe_context.hpp"

#include <cstddef>

namespace ohao::diff::probe {

bool checkArenaAtomicsAndStage(ohao::diff::GpuProbeContext& ctx,
                               const ohao::diff::ArenaLayout& layout,
                               ohao::diff::GradientArena& arena,
                               std::size_t blockA, std::size_t blockB,
                               std::size_t blockC);

bool checkRayQueryVisibility(ohao::diff::GpuProbeContext& ctx);

bool checkRegistryArenaSeam(ohao::diff::GpuProbeContext& ctx);

bool checkRngParity(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
