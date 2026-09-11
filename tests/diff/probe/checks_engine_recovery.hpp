// Stage 5, check 67: GATE 5 THROUGH THE FACADE, AGAINST AN ENGINE-OWNED VALUE.
//
// Check 54 recovers an albedo with the harness owning everything: its own
// registry, its own arena, its own host-side Adam over a `std::vector<float>`
// that nothing but the check can see. This is the same recovery, the same
// scene and THE SAME PRE-REGISTERED CRITERION, with the two ends moved:
//
//   * `DiffRenderer` owns the registry and the arena. The backward pass
//     writes its gradient into that arena at the offset the REGISTRY
//     assigned, and `DiffRenderer::recordAdamStep` reads it back out from the
//     same offset. The gradient never round-trips through the host at all,
//     which is the point of the arena having both blocks.
//
//   * The value lives in an engine-style owner -- a CPU-side authority with a
//     device buffer derived from it by whole-array copy, which is
//     `PathTracer::setMaterialData`'s shape. The optimiser writes the device
//     buffer; the host reads it back into the authority; the next forward
//     render reads the authority.
//
// THE CRITERION IS CHECK 54'S, UNCHANGED, and deliberately so. Same theta*,
// same theta_0, same iteration count, same alpha, same tolerance. Reusing it
// is what makes a difference in outcome attributable to the facade rather
// than to a number quietly chosen to suit the new path -- and retuning a
// tolerance while moving the code under it is exactly the tell this
// subsystem's checks are written to avoid.
//
// THE CONTROL IS CHECK 66'S FAILURE, PROMOTED. Check 66 showed that leaving
// the CPU authority stale loses one value to the next unrelated edit. Here
// the same omission is shown to break the OPTIMISATION: with no sync, every
// forward render reads the unchanged authority, so the loss never falls and
// theta never moves. A stale authority is not a tidiness problem; it severs
// the loop.
//
// WHAT THIS STILL DOES NOT DO. The forward and backward dispatches are still
// `runWavefrontGradientProbe`'s -- 640 lines of orchestration that live in
// the harness, not the library. `DiffRenderer` owns the parameters, the
// arena and the optimiser at this point, not the render. Moving that is the
// remainder of engine integration, and until it moves, this gate proves the
// ENDS are connected rather than the whole.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkEngineRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
