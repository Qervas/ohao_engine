#include "diff/geom/edge_adjacency.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace ohao::diff {

bool EdgeAdjacency::build(const std::vector<std::uint32_t>& indices) {
    m_edges.clear();
    m_error = "";
    if (indices.empty() || indices.size() % 3u != 0u) {
        m_error = "index count must be a non-zero multiple of 3";
        return false;
    }

    // Keyed by the CANONICAL (min, max) pair, so (a,b) and (b,a) land on one
    // entry. The value records the face and whether that face traversed the
    // edge in the canonical direction -- which is what the orientation test
    // below compares.
    struct Slot {
        std::uint32_t face0{MeshEdge::kNoFace};
        std::uint32_t face1{MeshEdge::kNoFace};
        bool forward0{false};
        bool forward1{false};
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, Slot> slots;

    const std::size_t faceCount = indices.size() / 3u;
    for (std::size_t f = 0; f < faceCount; ++f) {
        const std::uint32_t a = indices[f * 3u + 0u];
        const std::uint32_t b = indices[f * 3u + 1u];
        const std::uint32_t c = indices[f * 3u + 2u];
        const std::uint32_t tri[3][2] = {{a, b}, {b, c}, {c, a}};
        for (const auto& e : tri) {
            const bool forward = e[0] < e[1];
            const auto key = forward ? std::make_pair(e[0], e[1]) : std::make_pair(e[1], e[0]);
            Slot& slot = slots[key];
            if (slot.face0 == MeshEdge::kNoFace) {
                slot.face0 = static_cast<std::uint32_t>(f);
                slot.forward0 = forward;
            } else if (slot.face1 == MeshEdge::kNoFace) {
                slot.face1 = static_cast<std::uint32_t>(f);
                slot.forward1 = forward;
            } else {
                // NON-MANIFOLD. Refused rather than truncated: the
                // front/back-facing test in Task 2 asks about TWO faces, and
                // an edge with three has no answer. Silently keeping the
                // first two would give one.
                m_error = "an edge is shared by more than two faces (non-manifold)";
                m_edges.clear();
                return false;
            }
        }
    }

    m_edges.reserve(slots.size());
    for (const auto& [key, slot] : slots) {
        MeshEdge edge;
        edge.v0 = key.first;
        edge.v1 = key.second;
        edge.face0 = slot.face0;
        edge.face1 = slot.face1;
        // Opposite traversal means exactly one of the two faces walked the
        // edge in the canonical direction.
        edge.oppositelyOriented =
            (slot.face1 != MeshEdge::kNoFace) && (slot.forward0 != slot.forward1);
        m_edges.push_back(edge);
    }
    return true;
}

std::size_t EdgeAdjacency::boundaryEdgeCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_edges.begin(), m_edges.end(),
                      [](const MeshEdge& e) { return e.isBoundary(); }));
}

}  // namespace ohao::diff
