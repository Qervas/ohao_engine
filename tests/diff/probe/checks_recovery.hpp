// Stage 2 Task 5, check 54: GATE 5 -- recovery on synthetic ground truth.
//
// Spec 8.5: render with a known theta*, start from theta_0, optimise, assert
// theta -> theta*. This is the gate the whole stage exists to pass, and it is
// the first check in this probe that exercises every piece at once -- the
// forward render, the loss kernel, the adjoint seed, the backward pass and
// Adam, in a closed loop.
//
// THE CRITERION IS PRE-REGISTERED, and stated here rather than in the
// reporting, because a criterion chosen after seeing the trajectory is not a
// gate:
//
//     parameter   the albedo (DIFF_PARAM_BASECOLOR), the scalar with the
//                 best-conditioned gradient in the stage
//     theta*      0.6      -- the value every Stage 1 gradient check uses
//     theta_0     0.3      -- half of it, so a recovery cannot be a rounding
//                             artefact of starting near the answer
//     optimiser   Adam at alpha = 0.01, the paper's betas, 100 iterations
//     RECOVERED   |theta_final - theta*| <= 0.03
//
// WHERE 0.03 COMES FROM. It is 3 * alpha. Adam's effective step stays near
// alpha until the gradient's sign starts alternating, after which theta
// oscillates about the optimum with an amplitude of roughly alpha -- so a
// tolerance below alpha would be unmeetable by construction, however correct
// the gradient. Three times it is the smallest round multiple that is not
// measuring the oscillation itself. The available travel is 100 * 0.01 = 1.0
// against a distance of 0.3, so the budget is not what binds.
//
// WHY THE LOSS HAS AN EXACT ZERO HERE. The target is rendered at theta* with
// the SAME seed the optimisation uses, so under common random numbers
// L(theta*) = 0 exactly rather than at the level of Monte Carlo noise. The
// minimiser is therefore theta* itself and not a noisy neighbourhood of it,
// which is what lets the criterion be an absolute distance.
//
// IF THIS FAILS, IT MUST BE ATTRIBUTED, not merely reported. Checks 37-53
// pass on every piece separately, so a failure here is the LOOP -- unless it
// is the detached-sampling bias (spec 6.3, measured by check 41 at 1-4x the
// gradient and sign-flipping near-specular), which is a property of the
// method rather than a defect. The albedo is deliberately the first
// parameter tried precisely because it is the one where that bias is ABSENT:
// at metallic 0 the base colour enters no sampling decision at all, which is
// the precondition runWavefrontGradientProbe refuses to run without. So a
// failure on THIS parameter cannot be blamed on detached sampling.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
