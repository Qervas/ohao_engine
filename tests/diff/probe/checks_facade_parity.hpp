// Stage 5, check 65: THE FACADE COMPUTES WHAT THE PROBE COMPUTES.
//
// The gate that makes the rest of engine integration safe, and it runs before
// any engine resource is involved.
//
// Everything through Stage 3 is validated against buffers the PROBE creates,
// with the probe performing the sequence: zero the arena, dispatch, seed the
// adjoint, step Adam. `DiffRenderer` now performs part of that sequence
// itself, from the arena rather than from standalone buffers. If the two ever
// disagree, every later integration failure has two candidate causes and no
// way to separate them.
//
// So this check pins one to the other. `runAdamProbe` is what check 52 gates
// against Kingma & Ba's own algorithm listing; `DiffRenderer::recordAdamStep`
// reads the gradient and the state out of the ARENA at registered offsets and
// writes the values in place. Same hyperparameters, same gradient, same number
// of steps, and the results are compared as EXACT floats -- not a tolerance.
// Both run the identical SPIR-V on the identical input; a difference of one
// ulp would mean the two are not feeding it the same numbers, which is the
// only thing this check is here to detect.
//
// THE CONTROL is the arena's own hazard: `GradientArena::build` does not zero,
// so the optimiser state starts as whatever the allocator handed back. The
// check runs once WITHOUT clearing that state and requires the answer to
// differ -- which proves the state offsets are read at all, and re-establishes
// for this path the thing check 61 established for the boundary pass.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkFacadeParity(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
