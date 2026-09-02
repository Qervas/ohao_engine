#include "diff/geom/silhouette_set.hpp"

#include <cmath>
#include <map>
#include <set>

namespace ohao::diff {
namespace {

struct Vec3 {
    float x, y, z;
};

Vec3 vertexAt(const std::vector<float>& positions, std::uint32_t v) {
    return {positions[v * 3u + 0u], positions[v * 3u + 1u], positions[v * 3u + 2u]};
}

/// The face's geometric normal, from its winding -- cross(v1-v0, v2-v0).
/// NOT normalised: only the SIGN of its dot product is read below, and
/// normalising would divide by a length that is zero for a degenerate
/// triangle, turning a detectable problem into a NaN.
Vec3 faceNormal(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices,
                std::uint32_t face) {
    const Vec3 a = vertexAt(positions, indices[face * 3u + 0u]);
    const Vec3 b = vertexAt(positions, indices[face * 3u + 1u]);
    const Vec3 c = vertexAt(positions, indices[face * 3u + 2u]);
    const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    return {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
}

/// Is `face` front-facing from `cameraPos`? dot(n, camera - anyPointOnFace).
/// The point used is the face's first vertex, which is on the plane, so the
/// sign is the plane's own half-space test and does not depend on which
/// vertex was picked.
bool isFrontFacing(const std::vector<float>& positions,
                   const std::vector<std::uint32_t>& indices, std::uint32_t face,
                   const float cameraPos[3]) {
    const Vec3 n = faceNormal(positions, indices, face);
    const Vec3 p = vertexAt(positions, indices[face * 3u + 0u]);
    const float dx = cameraPos[0] - p.x;
    const float dy = cameraPos[1] - p.y;
    const float dz = cameraPos[2] - p.z;
    return (n.x * dx + n.y * dy + n.z * dz) > 0.0f;
}

}  // namespace

bool SilhouetteSet::build(const EdgeAdjacency& adjacency, const std::vector<float>& positions,
                          const std::vector<std::uint32_t>& indices, const float cameraPos[3]) {
    m_marked.clear();
    m_error = "";
    if (positions.empty() || positions.size() % 3u != 0u) {
        m_error = "positions must be a non-zero multiple of 3 floats";
        return false;
    }
    if (indices.empty() || indices.size() % 3u != 0u) {
        m_error = "indices must be a non-zero multiple of 3";
        return false;
    }
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(positions.size() / 3u);
    for (std::uint32_t i : indices) {
        if (i >= vertexCount) {
            m_error = "an index is out of range for the position array";
            return false;
        }
    }

    for (std::size_t e = 0; e < adjacency.edges().size(); ++e) {
        const MeshEdge& edge = adjacency.edges()[e];
        // OPEN BOUNDARY EDGES ARE ALWAYS SILHOUETTE (spec 7.1). There is no
        // second face to disagree with, so the surface simply stops here.
        if (edge.isBoundary()) {
            m_marked.push_back(static_cast<std::uint32_t>(e));
            continue;
        }
        const bool f0 = isFrontFacing(positions, indices, edge.face0, cameraPos);
        const bool f1 = isFrontFacing(positions, indices, edge.face1, cameraPos);
        if (f0 != f1) m_marked.push_back(static_cast<std::uint32_t>(e));
    }
    return true;
}

bool SilhouetteSet::isSingleClosedLoop(const EdgeAdjacency& adjacency,
                                       const char** reason) const {
    auto fail = [&](const char* why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (m_marked.empty()) return fail("the marked set is empty");

    // DEGREE: every endpoint of a marked edge must be met by exactly two
    // marked edges. Necessary for a cycle, and not sufficient -- two
    // disjoint loops satisfy it, which is why the walk below follows.
    std::map<std::uint32_t, int> degree;
    for (std::uint32_t e : m_marked) {
        ++degree[adjacency.edges()[e].v0];
        ++degree[adjacency.edges()[e].v1];
    }
    for (const auto& [vertex, d] : degree) {
        (void)vertex;
        if (d != 2) return fail("a vertex of the marked set has degree other than 2");
    }

    // CONNECTEDNESS: walk from one marked edge and require the walk to reach
    // every other. A hexagonal silhouette and two disjoint triangles both
    // pass the degree test; only one of them is a silhouette.
    std::map<std::uint32_t, std::vector<std::uint32_t>> incident;
    for (std::uint32_t e : m_marked) {
        incident[adjacency.edges()[e].v0].push_back(e);
        incident[adjacency.edges()[e].v1].push_back(e);
    }
    std::set<std::uint32_t> seen;
    std::vector<std::uint32_t> stack{m_marked.front()};
    while (!stack.empty()) {
        const std::uint32_t e = stack.back();
        stack.pop_back();
        if (!seen.insert(e).second) continue;
        for (std::uint32_t v : {adjacency.edges()[e].v0, adjacency.edges()[e].v1}) {
            for (std::uint32_t next : incident[v]) {
                if (seen.count(next) == 0) stack.push_back(next);
            }
        }
    }
    if (seen.size() != m_marked.size()) {
        return fail("the marked set splits into more than one loop");
    }
    if (reason != nullptr) *reason = "";
    return true;
}

}  // namespace ohao::diff
