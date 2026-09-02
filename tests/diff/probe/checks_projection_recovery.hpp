// Stage 3, check 63: A WORLD-SPACE TRANSLATION, RECOVERED THROUGH THE
// PROJECTION -- including its DEPTH component.
//
// The boundary pass returns dL/d(screen position). Checks 60 and 62 stop
// there and optimise screen-space quantities, which is why the Stage 3
// results note carries "orthographic only (no projection Jacobian)" as a
// deviation. This gate is that deviation closed end to end: the optimised
// parameter is a translation in WORLD space, and the gradient reaches it
// through PinholeProjection's pullback.
//
// THE DEPTH COMPONENT IS THE POINT. Under an orthographic camera the third
// column of the projection Jacobian is exactly zero, so a motion along the
// view direction is not merely hard to fit -- it is UNIDENTIFIABLE, and no
// optimiser, tolerance or iteration count can recover it. Perspective makes
// that column nonzero because moving away shrinks the projected shape.
//
// So the control writes itself, and it is the system's own previous state:
// the identical run with the depth column zeroed -- which is what the code
// did before PinholeProjection existed. It must leave tz EXACTLY where it
// started, because a zero gradient in Adam is a zero step, and it must
// finish at a visibly worse loss.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkProjectionRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
