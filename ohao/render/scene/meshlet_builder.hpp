#pragma once
// Meshlets (sub-project 2b spec, "Meshlets and mesh shaders"): a submesh's triangles in
// clusters of at most gpu::shared::kMeshletMaxVertices vertices and kMeshletMaxTriangles
// triangles, each with a bounding sphere and a normal cone. The mesh-shader path and its
// compute fallback cull and draw them; BLAS building keeps the original triangles.
//
// The builder is greedy and deterministic: it starts a meshlet at the first unused triangle
// in index order, then keeps adding the unused triangle that shares the most vertices with
// the meshlet (the oldest such candidate first, so a meshlet grows as a compact patch, not a
// strip), skipping triangles whose normal strays more than ~53 degrees from the meshlet's
// mean (so its normal cone can cull); with no neighbour left it takes the next unused
// triangle in index order; it closes the meshlet when the next triangle no longer fits.
// Every triangle lands in exactly one meshlet. One function, so a better clusterer can
// replace it.
#include "asset/model.hpp"   // Vertex

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace ohao::render {

struct Meshlet {
    uint32_t vertexOffset{0};     // into MeshletSet::vertices
    uint32_t triangleOffset{0};   // into MeshletSet::triangles
    uint32_t vertexCount{0};
    uint32_t triangleCount{0};
    glm::vec3 center{0.0f};       // bounding sphere, mesh space
    float radius{0.0f};
    glm::vec3 coneAxis{0.0f, 0.0f, 1.0f};
    float coneCutoff{1.0f};       // 1: never back-facing as a whole
};

struct MeshletSet {
    std::vector<Meshlet> meshlets;
    std::vector<uint32_t> vertices;    // mesh-local vertex indices, per meshlet
    std::vector<uint32_t> triangles;   // per triangle: its three meshlet-local indices, a | b << 8 | c << 16
};

// Appends the meshlets of indices [first, first + count) (mesh-local, into `vertices`) to
// `out`; offsets are relative to `out`'s arrays as they were. `count` is a multiple of 3.
void buildMeshlets(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint32_t first,
                   uint32_t count, MeshletSet& out);

} // namespace ohao::render
