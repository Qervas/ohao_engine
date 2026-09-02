// Stage 3, check 58: the silhouette pass on the GPU.
#include "probe/checks_silhouette_gpu.hpp"

#include "diff/geom/edge_adjacency.hpp"
#include "diff/geom/silhouette_set.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

/// A WELDED cube: 8 shared corners, 12 triangles, every edge used twice,
/// every face wound outward. Welded rather than the probe's own box because
/// that one gives each face its own vertices -- 24 of its 30 edges are OPEN,
/// and spec 7.1 marks open edges unconditionally, so its silhouette would be
/// the same for every camera and the view-dependence assertion below would
/// fail for a reason that has nothing to do with this pass. That was
/// measured in diff_unit_tests, not discovered here.
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

bool checkSilhouetteGpu(ohao::diff::GpuProbeContext& ctx) {
    const std::vector<std::uint32_t> indices = weldedCubeIndices();
    const std::vector<float> positions = weldedCubePositions();

    ohao::diff::EdgeAdjacency adjacency;
    if (!adjacency.build(indices)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 58 adjacency build: %s\n",
                     adjacency.error());
        return false;
    }
    // Flattened for the GPU: v0, v1, face0, face1 per edge. face1 keeps
    // MeshEdge::kNoFace's 0xFFFFFFFF, which the shader reads as "open".
    std::vector<std::uint32_t> records;
    records.reserve(adjacency.edgeCount() * 4u);
    for (const auto& e : adjacency.edges()) {
        records.push_back(e.v0);
        records.push_back(e.v1);
        records.push_back(e.face0);
        records.push_back(e.face1);
    }

    struct View {
        const char* name;
        float pos[3];
        std::size_t expected;  // derived on paper, not read off the pass
    };
    // Down the (1,1,1) diagonal three faces are front-facing and three back,
    // and the boundary between two connected sets of three cube faces is a
    // HEXAGON. Nearly face-on to +x, one face is front and five back: four
    // edges. Both counts are facts about the cube and the direction.
    const View views[2] = {
        {"the (1,1,1) corner", {3.0f, 3.0f, 3.0f}, 6u},
        {"nearly face-on to +x", {5.0f, 0.1f, 0.1f}, 4u},
    };

    std::vector<std::uint32_t> markedPerView[2];
    for (int v = 0; v < 2; ++v) {
        std::vector<std::uint32_t> flags;
        std::uint32_t count = 0u;
        if (!ctx.runSilhouetteProbe(positions, records, indices, views[v].pos, flags, count)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 58 dispatch for %s\n",
                         views[v].name);
            return false;
        }
        if (flags.size() != adjacency.edgeCount()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 58 -- %zu flags for %zu edges\n",
                         flags.size(), adjacency.edgeCount());
            return false;
        }

        // --- THE COUNT AGREES WITH THE FLAGS. An atomic counter and a flag
        // array are two independent writes of one fact; a disagreement means
        // one of them is racing or the predicate ran twice for some edge.
        std::size_t flagged = 0;
        for (std::uint32_t f : flags) flagged += (f != 0u) ? 1u : 0u;
        if (flagged != count) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 58 -- %zu edges carry a set flag but the "
                         "atomic counter says %u. These are two independent writes of one fact, "
                         "so a disagreement is a race or a predicate that ran twice for an "
                         "edge\n",
                         flagged, count);
            return false;
        }

        // --- THE COUNT IS THE ONE DERIVED ON PAPER.
        if (flagged != views[v].expected) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 58 -- from %s the silhouette has %zu "
                         "edges and the geometry says %zu. Down the (1,1,1) diagonal three cube "
                         "faces face the camera and three do not, and the boundary between two "
                         "connected sets of three faces is a hexagon; nearly face-on, one face "
                         "faces the camera and five do not, so the silhouette is that face's "
                         "four edges\n",
                         views[v].name, flagged, views[v].expected);
            return false;
        }

        // --- EXACT SET EQUALITY WITH THE HOST PASS, which diff_unit_tests
        // gates against the single-closed-loop invariant. A count can match
        // while the WRONG edges are marked; this cannot.
        ohao::diff::SilhouetteSet host;
        if (!host.build(adjacency, positions, indices, views[v].pos)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 58 host build: %s\n",
                         host.error());
            return false;
        }
        std::vector<std::uint32_t> gpuMarked;
        for (std::size_t e = 0; e < flags.size(); ++e) {
            if (flags[e] != 0u) gpuMarked.push_back(static_cast<std::uint32_t>(e));
        }
        if (gpuMarked != host.markedEdges()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 58 -- from %s the GPU marked a different "
                         "SET of edges than the host pass, though %zu of them either way. The "
                         "host pass is gated by diff_unit_tests against the single-closed-loop "
                         "invariant, so a disagreement here is this dispatch -- the winding it "
                         "reads, the face indices, or the boundary sentinel\n",
                         views[v].name, gpuMarked.size());
            return false;
        }

        // --- AND THAT SET IS A SINGLE CLOSED LOOP.
        //
        // Evaluated on `host`, and that is legitimate ONLY because the
        // assertion immediately above has already established that the GPU
        // marked the identical set -- so the invariant transfers to it. An
        // earlier version of this block built a SECOND host set and called it
        // "the GPU's": the same computation twice wearing a different name,
        // which would have held for a GPU pass that marked nothing at all had
        // the set-equality assertion not been there to stop it first.
        const char* why = "";
        if (!host.isSingleClosedLoop(adjacency, &why)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 58 -- from %s the marked set is not a "
                         "single closed loop: %s. For a convex closed mesh seen from outside "
                         "the front- and back-facing sets are each connected and share one "
                         "boundary, so the silhouette must be one cycle\n",
                         views[v].name, why);
            return false;
        }
        markedPerView[v] = gpuMarked;
    }

    // --- VIEW DEPENDENCE. Without it every assertion above holds for a pass
    // that ignores the camera entirely and marks a fixed set -- and on a mesh
    // whose faces do not share vertices, that is exactly what a correct pass
    // DOES, which is why the welded cube is used here.
    if (markedPerView[0] == markedPerView[1]) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 58 -- two cameras gave the SAME marked set, "
                     "so the pass may not be reading the view at all\n");
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 58 -- THE SILHOUETTE PASS ON THE GPU, spec 7.1's per-view "
        "half: one invocation per edge, marking where one adjacent face faces the camera and "
        "the other does not, plus open boundary edges. Two views over a welded cube, and the "
        "counts are DERIVED ON PAPER rather than read off the pass -- %zu edges down the "
        "(1,1,1) diagonal, where three faces face the camera and three do not so their boundary "
        "is a hexagon, and %zu nearly face-on to +x, where one face does and five do not. Four "
        "things are asserted at each view: the atomic count agrees with the flag array (two "
        "independent writes of one fact, so a disagreement is a race); the count is the "
        "predicted one; the SET is exactly the host pass's, which diff_unit_tests gates against "
        "the single-closed-loop invariant, because a count can match while the wrong edges are "
        "marked; and that invariant holds. Plus view dependence, without which all of it would "
        "hold for a pass that ignores the camera. THE MESH IS WELDED DELIBERATELY: the probe's "
        "own box gives every face its own vertices, so 24 of its 30 edges are OPEN and marked "
        "unconditionally, and its silhouette would be view-INDEPENDENT -- measured in the unit "
        "tests, not discovered here. FLAGS, NOT COMPACTION: a prefix sum's failure mode is a "
        "wrong offset, which is silent and looks like a missing edge, so it is a separate thing "
        "to check.\n",
        views[0].expected, views[1].expected);
    return true;
}

}  // namespace ohao::diff::probe
