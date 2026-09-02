// Stage 3, check 59: the two boundary passes connected.
#include "probe/checks_boundary_silhouette.hpp"

#include "diff/geom/edge_adjacency.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

namespace ohao::diff::probe {

namespace {

std::vector<std::uint32_t> weldedCubeIndices() {
    return {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5,
            0, 5, 4, 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2,
            1, 3, 7, 1, 7, 5};
}

std::vector<float> weldedCubePositions() {
    std::vector<float> p;
    for (std::uint32_t c = 0; c < 8u; ++c) {
        p.push_back((c & 1u) ? 1.0f : -1.0f);
        p.push_back((c & 2u) ? 1.0f : -1.0f);
        p.push_back((c & 4u) ? 1.0f : -1.0f);
    }
    return p;
}

}  // namespace

bool checkBoundaryOverSilhouette(ohao::diff::GpuProbeContext& ctx) {
    constexpr std::uint32_t kImage = 8u;
    constexpr float kLIn = 3.0f, kLOut = 0.5f;

    const std::vector<std::uint32_t> indices = weldedCubeIndices();
    const std::vector<float> world = weldedCubePositions();

    ohao::diff::EdgeAdjacency adjacency;
    if (!adjacency.build(indices)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 59 adjacency: %s\n",
                     adjacency.error());
        return false;
    }
    std::vector<std::uint32_t> records;
    std::vector<std::uint32_t> pairs;
    for (const auto& e : adjacency.edges()) {
        records.push_back(e.v0);
        records.push_back(e.v1);
        records.push_back(e.face0);
        records.push_back(e.face1);
        pairs.push_back(e.v0);
        pairs.push_back(e.v1);
    }

    // A camera down -z from outside, offset so no face is exactly edge-on --
    // where dot(n, c - p) is 0 and "front facing" is a coin toss. The +z face
    // is then the only one facing the camera, so the silhouette is its four
    // edges and its four vertices (4, 5, 6, 7) are the only ones on it.
    const float camera[3] = {0.1f, 0.15f, 10.0f};

    std::vector<std::uint32_t> flags;
    std::uint32_t marked = 0u;
    if (!ctx.runSilhouetteProbe(world, records, indices, camera, flags, marked)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 59 silhouette dispatch\n");
        return false;
    }
    if (marked != 4u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 59 -- from a camera down -z the only "
                     "front-facing cube face is +z, so the silhouette is its FOUR edges; the "
                     "pass marked %u\n",
                     marked);
        return false;
    }
    // WHICH VERTICES THE SILHOUETTE TOUCHES, read off the marked edges rather
    // than assumed from the camera. The null assertion below is about these.
    std::set<std::uint32_t> onSilhouette;
    for (std::size_t e = 0; e < flags.size(); ++e) {
        if (flags[e] == 0u) continue;
        onSilhouette.insert(adjacency.edges()[e].v0);
        onSilhouette.insert(adjacency.edges()[e].v1);
    }
    if (onSilhouette.size() != 4u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 59 -- the four marked edges touch %zu "
                     "vertices; a closed loop of four edges has four\n",
                     onSilhouette.size());
        return false;
    }

    // ORTHOGRAPHIC along -z: screen x,y ARE world x,y, mapped so the cube's
    // [-1,1] square lands on [2,6] of an 8x8 image -- inside it with margin,
    // so no edge is clipped by the image border and the test measures the
    // boundary term rather than the frame.
    std::vector<float> screen;
    for (std::uint32_t v = 0; v < 8u; ++v) {
        screen.push_back((world[v * 3u + 0u] + 1.0f) * 2.0f + 2.0f);
        screen.push_back((world[v * 3u + 1u] + 1.0f) * 2.0f + 2.0f);
    }

    std::vector<float> filtered;
    std::vector<float> unfiltered;
    if (!ctx.runBoundaryProbe(screen, pairs, kImage, kImage, {kLIn, kLOut}, flags, {}, nullptr, 0u, 0u, filtered) ||
        !ctx.runBoundaryProbe(screen, pairs, kImage, kImage, {kLIn, kLOut}, {}, {}, nullptr, 0u, 0u, unfiltered)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 59 boundary dispatch\n");
        return false;
    }

    // --- THE NULL TEST FOR THE BOUNDARY TERM. A vertex on NO silhouette edge
    // cannot receive a boundary contribution, and "cannot" is exact: the
    // scatter only ever writes the two endpoints of an edge it processed.
    // This is the geometric analogue of checks 38/43/47 -- and it is the one
    // assertion here that a wrong FILTER cannot survive, because an
    // unfiltered pass writes every vertex of every edge.
    std::size_t strayVertices = 0;
    std::size_t firstStray = 0;
    for (std::uint32_t v = 0; v < 8u; ++v) {
        if (onSilhouette.count(v) != 0u) continue;
        const bool moved = filtered[v * 2u + 0u] != 0.0f || filtered[v * 2u + 1u] != 0.0f;
        if (moved) {
            if (strayVertices == 0) firstStray = v;
            ++strayVertices;
        }
    }
    if (strayVertices != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 59 -- %zu vertices that lie on NO silhouette "
                     "edge received a boundary gradient; the first is vertex %zu at (%.9g, "
                     "%.9g). The scatter writes only the two endpoints of an edge it processed, "
                     "so a nonzero here means an unmarked edge was processed\n",
                     strayVertices, firstStray,
                     static_cast<double>(filtered[firstStray * 2u + 0u]),
                     static_cast<double>(filtered[firstStray * 2u + 1u]));
        return false;
    }

    // --- NON-VACUITY: the silhouette vertices DID receive something.
    std::size_t movedOnSilhouette = 0;
    for (std::uint32_t v : onSilhouette) {
        if (filtered[v * 2u + 0u] != 0.0f || filtered[v * 2u + 1u] != 0.0f) ++movedOnSilhouette;
    }
    if (movedOnSilhouette != onSilhouette.size()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 59 -- only %zu of the %zu silhouette vertices "
                     "received a gradient. All zeros would satisfy the null test above for the "
                     "wrong reason\n",
                     movedOnSilhouette, onSilhouette.size());
        return false;
    }

    // --- AND THE FILTER MATTERS. Without it the pass processes all 18 edges,
    // inventing a radiance jump across the 14 interior ones, where the
    // surface is the same on both sides and the boundary integrand is
    // genuinely zero. If the two runs agreed, the filter would be decoration.
    bool differs = false;
    for (std::size_t k = 0; k < filtered.size(); ++k) {
        if (filtered[k] != unfiltered[k]) differs = true;
    }
    if (!differs) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 59 -- filtering by the silhouette changed "
                     "nothing. The unfiltered pass processes all %zu edges and the filtered one "
                     "4, so agreeing means the flag is not being read and the null test above "
                     "passed for some other reason\n",
                     adjacency.edgeCount());
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 59 -- THE TWO STAGE 3 PASSES CONNECTED: the silhouette "
        "pass's flags drive the boundary pass. A welded cube seen down -z, where the +z face is "
        "the only one facing the camera, so the silhouette is its four edges and touches four "
        "of the eight vertices -- read off the MARKED EDGES rather than assumed from the "
        "camera. THE NULL TEST FOR THE BOUNDARY TERM: the other four vertices receive EXACTLY "
        "0.0f, compared as floats and not through a tolerance. That is the geometric analogue "
        "of checks 38/43/47, and it is the assertion a wrong filter cannot survive -- an "
        "unfiltered pass writes every vertex of every edge. FILTERING IS CORRECTNESS, NOT "
        "SPEED: an interior edge has the same surface on both sides, so its radiance jump is "
        "zero and evaluating it with a pushed jump invents a discontinuity the geometry does "
        "not have. Asserted directly, by running both ways over the same %zu edges and "
        "requiring them to DIFFER; the four silhouette vertices are separately required to have "
        "received something, without which all zeros would satisfy the null test for the wrong "
        "reason.\n",
        adjacency.edgeCount());
    return true;
}

}  // namespace ohao::diff::probe
