// Stage 5 / roadmap item 8, check 68: WHAT PRECONDITIONING IS FOR.
//
// `LaplacianVertexParameterisation`'s unit tests establish that it computes
// what it claims -- the pullback against a finite-difference oracle, lambda
// zero being exactly the identity, a concentrated gradient spreading further
// as lambda grows. None of that shows it is USEFUL. This does.
//
// THE SETUP IS CHOSEN SO THE DATA CANNOT DETERMINE THE ANSWER. A 24-vertex
// closed polygon against a 12x12 image: the silhouette crosses on the order
// of forty pixels, so there are more vertex components than there are
// constraints. That is not an artificial difficulty, it is the ordinary
// condition of geometry optimisation -- a mesh has far more vertices than an
// image has silhouette pixels -- and it is exactly where an unpreconditioned
// optimiser puts its unconstrained freedom into high-frequency noise, which
// is what "spiky" means.
//
// A shape whose vertices all sit ON the silhouette, with a well-determined
// target, would show nothing: every vertex would receive a gradient the data
// actually constrains, and preconditioning would have no unconstrained
// directions to suppress. So the underdetermination is the experiment, not a
// complication of it.
//
// THE CONTROL IS THE SAME RUN AT LAMBDA = 0, which the unit tests establish
// is EXACTLY the identity parameterisation. Identical code path, identical
// iterations, identical step size -- so a difference in outcome is the
// preconditioner and not the plumbing around it.
//
// TWO THINGS MUST BOTH HOLD, and the second is the one that matters. The
// preconditioned run must fit at least as well (loss), and it must arrive at
// a SMOOTHER shape (mean squared discrete Laplacian of the recovered
// polygon). Fitting better alone would be unremarkable; the claim of the
// method is that it reaches a usable mesh, and "usable" is the smoothness.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkPreconditioning(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
