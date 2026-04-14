#pragma once
#include <cstdint>

namespace ohao::rt {

// Ray tracing visibility masks — control which rays see which geometry.
// Vulkan rule: ray intersects instance if (ray_mask & instance_mask) != 0.
//
// Bit layout:
//   Bit 0: GI rays + shadow rays (static geometry only)
//   Bit 1-7: reserved for future ray types (reflection, AO, etc.)

// Instance masks (set per TLAS instance)
constexpr uint32_t MASK_VISIBLE_ALL    = 0xFF;  // visible to all ray types
constexpr uint32_t MASK_STATIC_ONLY    = 0xFF;  // static geometry — visible to everything
constexpr uint32_t MASK_ANIMATED       = 0xFE;  // animated geometry — invisible to GI/shadow (bit 0 clear)

// Ray masks (used in traceRayEXT calls)
constexpr uint32_t RAY_MASK_ALL        = 0xFF;  // hits everything
constexpr uint32_t RAY_MASK_STATIC     = 0x01;  // only hits static geometry (bit 0)

} // namespace ohao::rt
