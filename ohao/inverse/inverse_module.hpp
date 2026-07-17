#pragma once

// Inverse rendering — recover scene parameters by matching images.
//
// Pipeline:
//   θ → apply to scene → offline path trace → L(R(θ), I*) → finite-diff ∇L → update θ
//
// Core math:     image_loss, param_space, optimizer, quality
// App pipeline:  fit_config → scene_builder → render_session → staged_fit
//                → export_dataset / visual_polish → fit_engine

#include "inverse/image_loss.hpp"
#include "inverse/optimizer.hpp"
#include "inverse/param_space.hpp"
#include "inverse/quality.hpp"

#include "inverse/export_capture.hpp"
#include "inverse/export_dataset.hpp"
#include "inverse/fit_config.hpp"
#include "inverse/fit_engine.hpp"
#include "inverse/io.hpp"
#include "inverse/render_session.hpp"
#include "inverse/scene_builder.hpp"
#include "inverse/staged_fit.hpp"
#include "inverse/visual_polish.hpp"
