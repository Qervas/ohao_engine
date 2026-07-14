#pragma once

// Inverse rendering (Phase 1) — recover scene parameters by matching images.
//
// Pipeline:
//   θ → apply to scene → offline path trace → L(R(θ), I*) → finite-diff ∇L → update θ
//
// First wedge: material albedo recovery (fixed geometry + lights + camera).

#include "inverse/image_loss.hpp"
#include "inverse/optimizer.hpp"
#include "inverse/param_space.hpp"
#include "inverse/quality.hpp"
