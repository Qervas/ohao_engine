// STAGE 3 TASK 2 -- THE SILHOUETTE SET, recomputed per view per iteration.
//
// Spec 7.1: a pass over the adjacency list marking edges where one adjacent
// face is front-facing and the other is not, plus open boundary edges. That
// split -- topology once, silhouette per view -- is what makes per-iteration
// geometry optimisation affordable.
//
// THIS IS THE CPU FORM. The eventual pass is a compute dispatch, and this is
// what it will be checked against: two implementations of one definition,
// with the ORACLE being neither of them but the topological invariant below.
//
// THE INVARIANT THAT MAKES THIS CHECKABLE WITHOUT A SECOND IMPLEMENTATION.
// For a CONVEX CLOSED mesh viewed from outside, the front-facing and
// back-facing sets are each connected and share one boundary, so the
// silhouette is a SINGLE CLOSED LOOP: every marked edge meets each of its
// two endpoints with exactly one other marked edge. That is a statement
// about the output alone -- it does not name a single edge, and it cannot be
// satisfied by transcribing the marking rule.
//
// OPEN BOUNDARY EDGES ARE ALWAYS SILHOUETTE (spec 7.1), which has a
// consequence worth stating where it will be read: on a mesh whose faces do
// not share vertices, EVERY outer edge is a boundary edge and the silhouette
// is view-INDEPENDENT. tests/diff/context/probe_scene.hpp's box is exactly
// such a mesh -- six disconnected quads, 24 of its 30 edges open -- so it is
// not a scene this pass can be tested on. That was measured in Task 1's
// unit tests rather than discovered here.
#pragma once

#include "diff/geom/edge_adjacency.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ohao::diff {

/// Marks the silhouette of `adjacency` as seen from `cameraPos`.
///
/// `positions` is 3 floats per vertex and `indices` 3 per triangle -- the
/// same arrays the BLAS is built from, and the same ones `adjacency` was
/// built from. Positions enter HERE and not in EdgeAdjacency, which is what
/// keeps the topology reusable across iterations.
///
/// Returns the indices into `adjacency.edges()` of the marked edges, in
/// ascending order. Empty on malformed input; `error()` on the set says why.
class SilhouetteSet {
public:
    [[nodiscard]] bool build(const EdgeAdjacency& adjacency,
                             const std::vector<float>& positions,
                             const std::vector<std::uint32_t>& indices,
                             const float cameraPos[3]);

    [[nodiscard]] const std::vector<std::uint32_t>& markedEdges() const noexcept {
        return m_marked;
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_marked.size(); }
    [[nodiscard]] const char* error() const noexcept { return m_error; }

    /// Is the marked set a single closed loop? Every marked edge must meet
    /// each endpoint with exactly one other marked edge, and the whole set
    /// must be ONE cycle rather than several -- both are checked, because a
    /// pair of disjoint loops satisfies the degree condition alone.
    ///
    /// Returns false with a reason for a set that is empty, has a vertex of
    /// degree != 2, or splits into more than one cycle.
    [[nodiscard]] bool isSingleClosedLoop(const EdgeAdjacency& adjacency,
                                          const char** reason = nullptr) const;

private:
    std::vector<std::uint32_t> m_marked;
    const char* m_error{""};
};

}  // namespace ohao::diff
