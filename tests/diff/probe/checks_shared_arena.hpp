// Stage 3, check 61: the interior and boundary terms in ONE arena.
//
// Spec 4.1 asks for "two gradient contributions summed into the same arena",
// and nothing before this check made that true -- the boundary pass wrote a
// buffer of its own, so the two could not interfere and the regression gate
// over checks 37-55 held for free.
//
// Two parameters of DIFFERENT KINDS are registered together: an albedo
// ScalarBlock, whose boundary term is mathematically absent, and a
// VertexPositions block, the only kind for which it is not. The kind is
// asserted rather than assumed -- registering geometry as a ScalarBlock
// would erase the fact the split rests on.
//
// The null test is what makes sharing safe: every arena float outside the
// geometry block is EXACTLY 0.0f. A kernel writing at the wrong offset lands
// in an appearance parameter's block, where the damage would look like an
// ordinary gradient.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkSharedArena(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
