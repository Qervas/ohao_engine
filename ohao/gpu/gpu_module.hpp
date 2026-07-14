#pragma once

/**
 * Umbrella for the GPU / Vulkan subsystem (prefer specific headers in production).
 * Art bar: C++20 spans, Result/vk_check, designated create-infos, layout locks.
 */

#include "gpu/layout_meta.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "gpu/vulkan/material.hpp"
#include "gpu/vulkan/material_instance.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "gpu/vulkan/vk_utils.hpp"
