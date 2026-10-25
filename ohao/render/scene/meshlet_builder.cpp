#include "render/scene/meshlet_builder.hpp"
#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <thread>

namespace ohao::render {

namespace {

constexpr uint32_t kMaxVertices = gpu::shared::kMeshletMaxVertices;
constexpr uint32_t kMaxTriangles = gpu::shared::kMeshletMaxTriangles;
constexpr uint32_t kNone = 0xFFFFFFFFu;
// A triangle joins a meshlet only while its normal is within ~53 degrees of the meshlet's mean
// normal so far: meshlets stay flat enough for their normal cones to cull.
constexpr float kConeLimit = 0.6f;
// Face normals go over threads from this many triangles, on at most this many.
constexpr uint32_t kParallelTriangles = 65536;
constexpr uint32_t kMaxWorkers = 8;

// The meshlet's sphere (its vertices' box centre, the farthest vertex) and normal cone (the
// triangles' mean unit normal; the cutoff is sin of the widest angle to it, as
// meshoptimizer's: a cluster whose normals spread past ~84 degrees never culls). `normals`
// are its triangles' unit face normals (zero when degenerate: no facing to cull by), `sum`
// their sum.
void computeBounds(std::span<const Vertex> vertices, const MeshletSet& set, std::span<const glm::vec3> normals,
                   const glm::vec3& sum, Meshlet& m) {
    glm::vec3 mn(INFINITY), mx(-INFINITY);
    for (uint32_t i = 0; i < m.vertexCount; ++i) {
        const glm::vec3& p = vertices[set.vertices[m.vertexOffset + i]].position;
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    m.center = 0.5f * (mn + mx);
    float r2 = 0.0f;
    for (uint32_t i = 0; i < m.vertexCount; ++i) {
        const glm::vec3 d = vertices[set.vertices[m.vertexOffset + i]].position - m.center;
        r2 = std::max(r2, glm::dot(d, d));
    }
    m.radius = std::sqrt(r2);

    m.coneAxis = {0.0f, 0.0f, 1.0f};
    m.coneCutoff = 1.0f;
    const float sumLen = glm::length(sum);
    if (sumLen <= 1e-6f) return;   // no facing triangles, or they cancel
    const glm::vec3 axis = sum / sumLen;
    float minDot = 1.0f;
    for (const glm::vec3& n : normals)
        if (n.x != 0.0f || n.y != 0.0f || n.z != 0.0f) minDot = std::min(minDot, glm::dot(n, axis));
    m.coneAxis = axis;
    if (minDot > 0.1f) m.coneCutoff = std::sqrt(1.0f - minDot * minDot);
}

} // namespace

void buildMeshlets(std::span<const Vertex> vertices, std::span<const uint32_t> indices, uint32_t first,
                   uint32_t count, MeshletSet& out) {
    const uint32_t triCount = count / 3;
    if (triCount == 0) return;
    const uint32_t* idx = indices.data() + first;
    const auto vertexCount = static_cast<uint32_t>(vertices.size());

    // Vertex -> triangles (CSR): counts, then each vertex's end, filled backwards to its start.
    std::vector<uint32_t> adjStart(vertexCount + 1, 0u);
    for (uint32_t i = 0; i < triCount * 3; ++i) ++adjStart[idx[i] + 1];
    for (uint32_t v = 0; v < vertexCount; ++v) adjStart[v + 1] += adjStart[v];
    std::vector<uint32_t> adj(triCount * 3);
    for (uint32_t v = 0; v < vertexCount; ++v) adjStart[v] = adjStart[v + 1];
    for (uint32_t t = triCount; t-- > 0;)
        for (uint32_t k = 3; k-- > 0;) adj[--adjStart[idx[t * 3 + k]]] = t;
    adjStart[vertexCount] = triCount * 3;
    out.triangles.reserve(out.triangles.size() + triCount);
    out.vertices.reserve(out.vertices.size() + triCount);   // a closed mesh has about half as many

    // Unit face normals (zero for a degenerate triangle: it fits any meshlet). Each is its own
    // triangle's alone, so a large mesh splits them over threads (a million triangles: ~11 ms
    // on one); the values do not depend on the split.
    // Default-initialised (glm::vec3 is trivial: no zeroing pass), so the workers' first writes
    // fault its pages in, in parallel.
    const std::unique_ptr<glm::vec3[]> faceNormalStore = std::make_unique_for_overwrite<glm::vec3[]>(triCount);
    glm::vec3* const faceNormal = faceNormalStore.get();
    const auto normalsOf = [&](uint32_t begin, uint32_t end) {
        for (uint32_t t = begin; t < end; ++t) {
            const glm::vec3& a = vertices[idx[t * 3]].position;
            const glm::vec3 n = glm::cross(vertices[idx[t * 3 + 1]].position - a, vertices[idx[t * 3 + 2]].position - a);
            const float len = glm::length(n);
            faceNormal[t] = len > 1e-12f ? n / len : glm::vec3(0.0f);
        }
    };
    const uint32_t workers =
        triCount < kParallelTriangles ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, kMaxWorkers);
    if (workers == 1) {
        normalsOf(0, triCount);
    } else {
        std::vector<std::thread> pool;
        const uint32_t chunk = (triCount + workers - 1) / workers;
        for (uint32_t w = 1; w < workers; ++w)
            pool.emplace_back(normalsOf, std::min(triCount, w * chunk), std::min(triCount, (w + 1) * chunk));
        normalsOf(0, std::min(triCount, chunk));
        for (std::thread& th : pool) th.join();
    }

    std::vector<uint8_t> used(triCount, 0);
    std::vector<uint8_t> score(triCount, 0);       // the triangle's vertices already in the meshlet
    std::vector<uint32_t> localOf(vertexCount, kNone);
    std::vector<uint32_t> touched;                 // triangles whose score this meshlet raised
    std::array<std::vector<uint32_t>, 4> bucket;   // candidates by score (1..3), in the order they reached it
    std::array<std::size_t, 4> head{};             // each bucket's next candidate: oldest first
    uint32_t cursor = 0;                           // the first possibly unused triangle in order
    uint32_t emitted = 0;

    while (emitted < triCount) {
        Meshlet m;
        m.vertexOffset = static_cast<uint32_t>(out.vertices.size());
        m.triangleOffset = static_cast<uint32_t>(out.triangles.size());
        for (auto& b : bucket) b.clear();
        head.fill(0);
        glm::vec3 normalSum(0.0f);   // the meshlet's unit face normals, summed
        float normalLimit = 0.0f;    // kConeLimit * |normalSum|
        std::array<glm::vec3, kMaxTriangles> normals;   // the meshlet's, in order

        const auto newVertices = [&](uint32_t t) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t v = idx[t * 3 + k];
                bool seen = localOf[v] != kNone;
                for (uint32_t j = 0; j < k && !seen; ++j) seen = idx[t * 3 + j] == v;   // repeated within t
                n += seen ? 0u : 1u;
            }
            return n;
        };
        const auto add = [&](uint32_t t) {
            used[t] = 1;
            ++emitted;
            uint32_t local[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t v = idx[t * 3 + k];
                if (localOf[v] == kNone) {
                    localOf[v] = m.vertexCount++;
                    out.vertices.push_back(v);
                    for (uint32_t a = adjStart[v]; a < adjStart[v + 1]; ++a) {
                        const uint32_t u = adj[a];
                        if (used[u]) continue;
                        if (score[u] == 0) touched.push_back(u);
                        if (score[u] < 3) ++score[u];
                        bucket[score[u]].push_back(u);
                    }
                }
                local[k] = localOf[v];
            }
            out.triangles.push_back(local[0] | (local[1] << 8) | (local[2] << 16));
            normals[m.triangleCount++] = faceNormal[t];
            normalSum += faceNormal[t];
            normalLimit = kConeLimit * glm::length(normalSum);
        };
        const auto fits = [&](uint32_t t) {
            return m.triangleCount < kMaxTriangles && m.vertexCount + newVertices(t) <= kMaxVertices;
        };
        // A degenerate triangle (zero normal) fits any meshlet, and anything fits one whose
        // normals cancel so far (normalLimit 0).
        const auto coherent = [&](uint32_t t) {
            const glm::vec3& n = faceNormal[t];
            return normalLimit <= 1e-6f || (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f) ||
                   glm::dot(n, normalSum) >= normalLimit;
        };

        while (cursor < triCount && used[cursor]) ++cursor;
        add(cursor);
        for (;;) {
            // The neighbour sharing the most vertices (oldest first) whose normal keeps the
            // meshlet coherent; stale entries (used, or since raised to a higher bucket) and
            // incoherent ones are skipped for this meshlet.
            uint32_t next = kNone;
            for (int s = 3; s >= 1 && next == kNone; --s) {
                auto& b = bucket[s];
                while (head[s] < b.size()) {
                    const uint32_t t = b[head[s]++];
                    if (!used[t] && score[t] == s && coherent(t)) {
                        next = t;
                        break;
                    }
                }
            }
            if (next == kNone) {   // no neighbour: the next triangle in index order
                while (cursor < triCount && used[cursor]) ++cursor;
                if (cursor < triCount && coherent(cursor)) next = cursor;
            }
            if (next == kNone || !fits(next)) break;
            add(next);
        }

        for (uint32_t i = 0; i < m.vertexCount; ++i) localOf[out.vertices[m.vertexOffset + i]] = kNone;
        for (const uint32_t t : touched) score[t] = 0;
        touched.clear();
        computeBounds(vertices, out, std::span<const glm::vec3>(normals.data(), m.triangleCount), normalSum, m);
        out.meshlets.push_back(m);
    }
}

} // namespace ohao::render
