// STAGE 3 TASK 1 -- EDGE ADJACENCY, BUILT ONCE PER MESH EVER.
//
// Which edges exist, and which triangles share them. Spec 7.1:
//
//   `EdgeAdjacency` -- built once, topological. This does not change when
//   vertex positions are optimized. Moving a vertex changes geometry, not
//   connectivity.
//
// That invariance is the whole cost argument for per-iteration geometry
// optimisation, and it is asserted rather than assumed: the unit test builds
// this, moves every vertex, rebuilds, and requires an identical structure.
//
// CPU-SIDE AND INDEX-ONLY. Nothing here reads a vertex POSITION -- the type
// takes indices and never sees the position array at all, which is what
// makes the invariance claim true by construction rather than by luck. The
// silhouette pass (Task 2) is where positions enter, and it runs per view on
// the GPU over the structure this produces.
#pragma once

#include <cstdint>
#include <vector>

namespace ohao::diff {

/// One undirected edge, and the up-to-two triangles that use it.
struct MeshEdge {
    /// The two endpoints, ALWAYS with v0 < v1. Undirected edges need a
    /// canonical spelling or (a,b) and (b,a) become two edges, and the
    /// silhouette pass would then see every edge twice with one face each --
    /// which looks exactly like a mesh that is open everywhere.
    std::uint32_t v0{0};
    std::uint32_t v1{0};
    /// Triangle indices. `face1 == kNoFace` marks a BOUNDARY edge: one
    /// adjacent face, an open mesh. Spec 7.1 counts those as silhouette
    /// edges unconditionally, so the distinction is load-bearing rather
    /// than diagnostic.
    std::uint32_t face0{0};
    std::uint32_t face1{0};
    /// True when the edge appears in face0 as (v0, v1) and in face1 as
    /// (v1, v0) -- i.e. the two faces traverse it in OPPOSITE directions,
    /// which is what a consistently wound manifold does. A closed mesh with
    /// any face wound the wrong way has this false somewhere, and its
    /// front/back-facing test in Task 2 would then be reading a normal that
    /// points inward.
    bool oppositelyOriented{false};

    static constexpr std::uint32_t kNoFace = 0xFFFFFFFFu;

    [[nodiscard]] bool isBoundary() const noexcept { return face1 == kNoFace; }
};

class EdgeAdjacency {
public:
    /// `indices` is 3 per triangle, as the BLAS is built from. Returns false
    /// on a length that is not a multiple of 3, or on an edge shared by more
    /// than two faces -- non-manifold geometry, which the silhouette test
    /// has no meaning for and which would otherwise be silently truncated to
    /// the first two faces found.
    [[nodiscard]] bool build(const std::vector<std::uint32_t>& indices);

    [[nodiscard]] const std::vector<MeshEdge>& edges() const noexcept { return m_edges; }
    [[nodiscard]] std::size_t edgeCount() const noexcept { return m_edges.size(); }
    [[nodiscard]] std::size_t boundaryEdgeCount() const noexcept;
    [[nodiscard]] const char* error() const noexcept { return m_error; }

private:
    std::vector<MeshEdge> m_edges;
    const char* m_error{""};
};

}  // namespace ohao::diff
