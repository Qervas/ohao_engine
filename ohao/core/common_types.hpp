#pragma once

/**
 * Shared POD types used across GPU upload, scene, and mesh indexing.
 * Keep these trivially copyable — they cross CPU/GPU and serialization edges.
 */

#include "core/concepts.hpp"

#include <cstdint>

namespace ohao {

/// Sub-range of a packed vertex/index buffer belonging to one mesh/submesh.
struct MeshBufferInfo {
    uint32_t vertexOffset{0};  // offset in vertices (not bytes)
    uint32_t indexOffset{0};   // offset in indices (not bytes)
    uint32_t indexCount{0};    // number of indices for this mesh
    uint32_t vertexCount{0};   // number of vertices for this mesh (RT BLAS)

    [[nodiscard]] constexpr bool empty() const noexcept { return indexCount == 0; }
    [[nodiscard]] constexpr bool hasVertices() const noexcept { return vertexCount != 0; }

    /// Inclusive index range end (one past last index).
    [[nodiscard]] constexpr uint32_t indexEnd() const noexcept {
        return indexOffset + indexCount;
    }

    /// Inclusive vertex range end (one past last vertex).
    [[nodiscard]] constexpr uint32_t vertexEnd() const noexcept {
        return vertexOffset + vertexCount;
    }
};

OHAO_ASSERT_GPU_LAYOUT(MeshBufferInfo, 16);

/// Identity empty mesh slice (useful as default / sentinel).
inline constexpr MeshBufferInfo kEmptyMeshBuffer{};

/// Strong-ish type aliases for readability at API boundaries.
using ObjectId = std::uint64_t;
using FrameIndex = std::uint32_t;
using SampleIndex = std::uint32_t;

inline constexpr ObjectId kInvalidObjectId = 0;
inline constexpr FrameIndex kInvalidFrameIndex = ~FrameIndex{0};

} // namespace ohao
