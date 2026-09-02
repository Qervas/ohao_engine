// Stage 3 Task 5, check 60: GATE 5 FOR GEOMETRY.
//
// A triangle's six vertex components recovered from a synthetic target by
// descending the BOUNDARY TERM -- the stage's stated gate (spec 8.5 applied
// to geometry).
//
// THE INTERIOR TERM IS EXACTLY ZERO IN THIS SCENE, and that is what makes
// the gate attributable rather than merely green. Constant radiance on each
// side means moving a vertex changes nothing but coverage, so spec 4.1's
// interior integral vanishes and the gradient descended is purely the
// boundary term. Against a shaded scene the two would sum and a failure
// could not be blamed on either.
//
// Every piece of the loop is separately gated -- checks 57 and 59 for the
// boundary term, check 52 for Adam -- so a failure here would have been the
// LOOP: the seed reaching the pass, the sign of the step, or the order of
// the stages.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkGeometryRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
