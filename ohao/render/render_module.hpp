#pragma once

/**
 * Umbrella for non-RT render (deferred, graph, camera, post, particles, …).
 * RT path: prefer render/rt/rt_module.hpp
 */

#include "render/async/async_compute_queue.hpp"
#include "render/camera/camera.hpp"
#include "render/camera/scene_framer.hpp"
#include "render/culling.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"
#include "render/deferred/render_pass_base.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/render_graph.hpp"
#include "render/graph/resource_handle.hpp"
#include "render/ibl/ibl_processor.hpp"
#include "render/particles/particle_system.hpp"
#include "render/picking/picking_system.hpp"
#include "render/picking/ray.hpp"
