#pragma once

#include <cstdint>
struct MeshBufferInfo {
    uint32_t vertexOffset;   // offset in vertices (not bytes)
    uint32_t indexOffset;    // offset in indices (not bytes)
    uint32_t indexCount;     // number of indices for this mesh
    uint32_t vertexCount;    // number of vertices for this mesh (added for RT BLAS)
};
