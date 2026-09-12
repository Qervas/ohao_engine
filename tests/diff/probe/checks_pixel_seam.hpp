// Check 71: AN EDGE EXACTLY ON A PIXEL SEAM IS COUNTED ONCE.
//
// The boundary pass clips each edge to each pixel. An edge lying exactly
// along the seam between two pixels was a valid chord for BOTH of them --
// local x = 1 for the pixel on one side, local x = 0 for the pixel on the
// other -- so its contribution was scattered twice and the gradient came out
// at exactly double. `clipToPixel` now treats the pixel as half-open, [0,1)
// on each axis, so the pixel below-left of a seam keeps the edge and the one
// above-right rejects it.
//
// THIS CHECK EXISTS BECAUSE THE FIX IS OTHERWISE UNTESTED. Only the
// parallel-to-a-boundary case changes, every other chord is clipped exactly
// as before, and the existing scenes were deliberately moved OFF integer
// coordinates once the bug was understood -- so the whole suite reproduces
// byte for byte whether the fix is present or not. A correctness fix that no
// check can distinguish from its absence is not fixed, it is merely written.
//
// THE SCENE IS DEGENERATE ON PURPOSE: a quad whose four edges lie exactly on
// integer coordinates, which is what a hand-written test scene naturally
// looks like and is precisely why this went unnoticed. The oracle is a
// supersampled image derivative, which has no pixel clip in it at all -- and
// that is the only kind of check that could ever have seen this, because any
// comparison between two forms of this kernel shares the doubling and
// cancels it exactly.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkPixelSeam(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
