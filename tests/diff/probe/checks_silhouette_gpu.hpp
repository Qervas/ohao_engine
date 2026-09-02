// Stage 3, check 58: the silhouette pass on the GPU.
//
// Four assertions per view, and the third is the one a count cannot make:
// the marked SET must be exactly the host pass's, because a count can match
// while the wrong edges are marked. The host pass is gated by
// diff_unit_tests against the single-closed-loop invariant, so a
// disagreement localises to this dispatch rather than to the predicate.
//
// The mesh is a WELDED cube, deliberately. The probe's own box gives every
// face its own vertices, so 24 of its 30 edges are open, and spec 7.1 marks
// open edges unconditionally -- its silhouette is the same for every camera,
// and the view-dependence assertion would fail for a reason that has nothing
// to do with this pass. That was measured in the unit tests, not discovered
// here.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkSilhouetteGpu(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
