#pragma once

/**
 * Shared POD types used across GPU upload, scene, and mesh indexing.
 * Keep these trivially copyable — they cross CPU/GPU and serialization edges.
 */

#include "core/concepts.hpp"

#include <cstdint>

namespace ohao {

/// Strong-ish type aliases for readability at API boundaries.
using ObjectId = std::uint64_t;
using FrameIndex = std::uint32_t;
using SampleIndex = std::uint32_t;

inline constexpr ObjectId kInvalidObjectId = 0;
inline constexpr FrameIndex kInvalidFrameIndex = ~FrameIndex{0};

} // namespace ohao
