#include "navigation/private/navmesh.hpp"

#include "asset/content_hash.hpp"
#include "asset/cooked.hpp"
#include "core/jobs/graph.hpp"
#include "core/log.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourObstacleAvoidance.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace ohao::nav {

namespace {

// Raised when what a tile's cooked bytes hold changes: every tile's name changes with it.
constexpr uint32_t kTileFormat = 1;
constexpr uint32_t kTileMagic = 0x5456414eu;   // "NAVT"
constexpr int kMaxLayers = 8;                  // tile cache layers a tile may hold (heights stacked in one column)
constexpr int kVertsPerPoly = 6;
constexpr unsigned short kWalk = 1;            // the polygon flag every walkable polygon and link carries
constexpr unsigned char kLinkArea = 1;

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The layers are kept as they are built: they are a few kilobytes a tile, and the cooked cache is local.
struct CopyCompressor final : dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override { return bufferSize; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                      const int maxCompressed, int* compressedSize) override {
        if (bufferSize > maxCompressed) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        std::memcpy(compressed, buffer, static_cast<std::size_t>(bufferSize));
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                        const int maxBufferSize, int* bufferSize) override {
        if (compressedSize > maxBufferSize) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        std::memcpy(buffer, compressed, static_cast<std::size_t>(compressedSize));
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

// Each walkable polygon gets the walk flag; every link is offered to every tile (Detour keeps those that start in it).
struct MeshProcess final : dtTileCacheMeshProcess {
    std::vector<float> verts, rads;
    std::vector<unsigned char> dirs, areas;
    std::vector<unsigned short> flags;
    std::vector<unsigned int> ids;

    void set(std::span<const Link> links) {
        verts.clear(); rads.clear(); dirs.clear(); areas.clear(); flags.clear(); ids.clear();
        for (const Link& l : links) {
            verts.insert(verts.end(), {l.a.x, l.a.y, l.a.z, l.b.x, l.b.y, l.b.z});
            rads.push_back(l.radius);
            dirs.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
            areas.push_back(kLinkArea);
            flags.push_back(kWalk);
            ids.push_back(l.id);
        }
    }
    void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i)
            polyFlags[i] = polyAreas[i] == DT_TILECACHE_WALKABLE_AREA ? kWalk : 0;
        if (rads.empty()) return;
        params->offMeshConVerts = verts.data();
        params->offMeshConRad = rads.data();
        params->offMeshConDir = dirs.data();
        params->offMeshConAreas = areas.data();
        params->offMeshConFlags = flags.data();
        params->offMeshConUserID = ids.data();
        params->offMeshConCount = static_cast<int>(rads.size());
    }
};

struct TileInput {
    int tx{0}, ty{0};
    float bmin[3]{}, bmax[3]{};      // the tile with its border, y from its triangles
    std::vector<int> tris;           // indices into the geometry's vertices, three a triangle
    ContentHash key{};
    std::vector<std::vector<uint8_t>> layers;   // the cooked layers
    bool cookedNow{false};
};

rcConfig tileConfig(const Settings& s, const TileInput& t) {
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.slopeDeg;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / s.cellHeight));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentClimb / s.cellHeight));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / s.cellSize));
    cfg.maxEdgeLen = static_cast<int>(12.0f / s.cellSize);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = kVertsPerPoly;
    cfg.tileSize = s.tileCells;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + 2 * cfg.borderSize;
    cfg.height = cfg.tileSize + 2 * cfg.borderSize;
    rcVcopy(cfg.bmin, t.bmin);
    rcVcopy(cfg.bmax, t.bmax);
    return cfg;
}

// One tile's layers, voxelized from its triangles (Recast's tile cache recipe: rasterize, filter, erode by the agent's
// radius, partition into height layers). Empty when nothing in it is walkable.
bool cookTile(const Settings& s, const std::vector<glm::vec3>& verts, TileInput& t) {
    rcContext ctx(false);
    const rcConfig cfg = tileConfig(s, t);
    struct Owned {
        rcHeightfield* solid{rcAllocHeightfield()};
        rcCompactHeightfield* chf{rcAllocCompactHeightfield()};
        rcHeightfieldLayerSet* lset{rcAllocHeightfieldLayerSet()};
        ~Owned() {
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            rcFreeHeightfieldLayerSet(lset);
        }
    } o;
    if (!o.solid || !o.chf || !o.lset) return false;
    if (!rcCreateHeightfield(&ctx, *o.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return false;
    const int ntris = static_cast<int>(t.tris.size() / 3);
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    const float* v = &verts[0].x;
    const int nverts = static_cast<int>(verts.size());
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, v, nverts, t.tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, v, nverts, t.tris.data(), areas.data(), ntris, *o.solid, cfg.walkableClimb)) return false;
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *o.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *o.solid);
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *o.solid, *o.chf)) return false;
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *o.chf)) return false;
    if (!rcBuildHeightfieldLayers(&ctx, *o.chf, cfg.borderSize, cfg.walkableHeight, *o.lset)) return false;
    CopyCompressor comp;
    for (int i = 0; i < std::min(o.lset->nlayers, kMaxLayers); ++i) {
        const rcHeightfieldLayer& layer = o.lset->layers[i];
        dtTileCacheLayerHeader header{};
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = t.tx;
        header.ty = t.ty;
        header.tlayer = i;
        dtVcopy(header.bmin, layer.bmin);
        dtVcopy(header.bmax, layer.bmax);
        header.width = static_cast<unsigned char>(layer.width);
        header.height = static_cast<unsigned char>(layer.height);
        header.minx = static_cast<unsigned char>(layer.minx);
        header.maxx = static_cast<unsigned char>(layer.maxx);
        header.miny = static_cast<unsigned char>(layer.miny);
        header.maxy = static_cast<unsigned char>(layer.maxy);
        header.hmin = static_cast<unsigned short>(layer.hmin);
        header.hmax = static_cast<unsigned short>(layer.hmax);
        unsigned char* data = nullptr;
        int size = 0;
        if (dtStatusFailed(dtBuildTileCacheLayer(&comp, &header, layer.heights, layer.areas, layer.cons, &data, &size)))
            return false;
        t.layers.emplace_back(data, data + size);
        dtFree(data);
    }
    return true;
}

std::vector<uint8_t> tileFile(const TileInput& t) {
    std::vector<uint8_t> out;
    const auto put = [&](uint32_t x) {
        const auto* p = reinterpret_cast<const uint8_t*>(&x);
        out.insert(out.end(), p, p + 4);
    };
    put(kTileMagic);
    put(static_cast<uint32_t>(t.layers.size()));
    for (const auto& l : t.layers) {
        put(static_cast<uint32_t>(l.size()));
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

bool readTileFile(std::span<const uint8_t> bytes, TileInput& t) {
    std::size_t at = 0;
    const auto get = [&](uint32_t& x) {
        if (at + 4 > bytes.size()) return false;
        std::memcpy(&x, bytes.data() + at, 4);
        at += 4;
        return true;
    };
    uint32_t magic = 0, n = 0;
    if (!get(magic) || magic != kTileMagic || !get(n) || n > static_cast<uint32_t>(kMaxLayers)) return false;
    std::vector<std::vector<uint8_t>> layers;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t size = 0;
        if (!get(size) || at + size > bytes.size()) return false;
        layers.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    t.layers = std::move(layers);
    return true;
}

} // namespace

struct Mesh::Impl {
    Settings settings;
    dtNavMesh* navmesh{nullptr};
    dtTileCache* cache{nullptr};
    dtTileCacheAlloc alloc;
    CopyCompressor comp;
    MeshProcess process;
    uint64_t epoch{0};
    uint64_t builds{0};
    struct Placed {
        Box box;
        dtObstacleRef ref{0};
    };
    std::map<uint64_t, Placed> obstacles;   // by id

    void clear() {
        dtFreeTileCache(cache);
        cache = nullptr;
        dtFreeNavMesh(navmesh);
        navmesh = nullptr;
        obstacles.clear();
    }
    ~Impl() { clear(); }

    // Runs the tile cache until every request is applied and every touched tile rebuilt.
    void settle() {
        bool upToDate = false;
        for (int guard = 0; guard < 100000 && !upToDate; ++guard)
            if (dtStatusFailed(cache->update(0.0f, navmesh, &upToDate))) break;
    }
};

Mesh::Mesh() : m(std::make_unique<Impl>()) {}
Mesh::~Mesh() = default;

bool Mesh::empty() const noexcept { return m->navmesh == nullptr; }
const Settings& Mesh::settings() const noexcept { return m->settings; }
uint64_t Mesh::epoch() const noexcept { return m->epoch; }
uint32_t Mesh::obstacleCount() const noexcept { return static_cast<uint32_t>(m->obstacles.size()); }

uint32_t Mesh::polyCount() const {
    if (!m->navmesh) return 0;
    const dtNavMesh& nm = *m->navmesh;
    uint32_t n = 0;
    for (int i = 0; i < nm.getMaxTiles(); ++i) {
        const dtMeshTile* t = nm.getTile(i);
        if (t && t->header) n += static_cast<uint32_t>(t->header->polyCount - t->header->offMeshConCount);
    }
    return n;
}

void Mesh::outlines(std::vector<std::pair<glm::vec3, glm::vec3>>& edges,
                    std::vector<std::pair<glm::vec3, glm::vec3>>& links) const {
    if (!m->navmesh) return;
    const dtNavMesh& nm = *m->navmesh;
    for (int i = 0; i < nm.getMaxTiles(); ++i) {
        const dtMeshTile* t = nm.getTile(i);
        if (!t || !t->header) continue;
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            const auto at = [&](int k) {
                const float* v = &t->verts[poly.verts[k] * 3];
                return glm::vec3(v[0], v[1], v[2]);
            };
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                links.emplace_back(at(0), at(1));
                continue;
            }
            for (int k = 0; k < poly.vertCount; ++k) edges.emplace_back(at(k), at((k + 1) % poly.vertCount));
        }
    }
}

bool Mesh::build(const Geometry& g, const Settings& s, const std::filesystem::path& cacheDir, BuildReport& out) {
    const auto t0 = std::chrono::steady_clock::now();
    m->clear();
    m->settings = s;
    ++m->epoch;
    ++m->builds;
    out = BuildReport{};
    if (g.tris.empty() || g.verts.empty()) {
        out.loadMs = msSince(t0);
        return true;
    }
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& v : g.verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const float ts = s.tileSize();
    const float border = static_cast<float>(static_cast<int>(std::ceil(s.agentRadius / s.cellSize)) + 3) * s.cellSize;
    const float origX = std::floor(lo.x / ts) * ts, origZ = std::floor(lo.z / ts) * ts;
    const int tw = std::max(1, static_cast<int>(std::ceil((hi.x - origX) / ts)));
    const int th = std::max(1, static_cast<int>(std::ceil((hi.z - origZ) / ts)));
    if (static_cast<int64_t>(tw) * th > 4096) {
        out.warnings.push_back("the level spans " + std::to_string(tw) + " x " + std::to_string(th) +
                               " tiles, more than 4096; no navmesh");
        out.loadMs = msSince(t0);
        return false;
    }

    // Each triangle into every tile its bounds (with the border) overlap, in triangle order.
    std::vector<TileInput> tiles(static_cast<std::size_t>(tw * th));
    for (int ty = 0; ty < th; ++ty)
        for (int tx = 0; tx < tw; ++tx) {
            TileInput& t = tiles[static_cast<std::size_t>(ty * tw + tx)];
            t.tx = tx;
            t.ty = ty;
            t.bmin[0] = origX + tx * ts - border;
            t.bmin[2] = origZ + ty * ts - border;
            t.bmax[0] = origX + (tx + 1) * ts + border;
            t.bmax[2] = origZ + (ty + 1) * ts + border;
            t.bmin[1] = 1e30f;
            t.bmax[1] = -1e30f;
        }
    const std::size_t ntris = g.tris.size() / 3;
    for (std::size_t i = 0; i < ntris; ++i) {
        const glm::vec3& a = g.verts[g.tris[i * 3]];
        const glm::vec3& b = g.verts[g.tris[i * 3 + 1]];
        const glm::vec3& c = g.verts[g.tris[i * 3 + 2]];
        const glm::vec3 tlo = glm::min(a, glm::min(b, c)), thi = glm::max(a, glm::max(b, c));
        const int x0 = std::max(0, static_cast<int>(std::floor((tlo.x - border - origX) / ts)));
        const int x1 = std::min(tw - 1, static_cast<int>(std::floor((thi.x + border - origX) / ts)));
        const int z0 = std::max(0, static_cast<int>(std::floor((tlo.z - border - origZ) / ts)));
        const int z1 = std::min(th - 1, static_cast<int>(std::floor((thi.z + border - origZ) / ts)));
        for (int tz = z0; tz <= z1; ++tz)
            for (int tx = x0; tx <= x1; ++tx) {
                TileInput& t = tiles[static_cast<std::size_t>(tz * tw + tx)];
                if (tlo.x > t.bmax[0] || thi.x < t.bmin[0] || tlo.z > t.bmax[2] || thi.z < t.bmin[2]) continue;
                t.tris.insert(t.tris.end(), {static_cast<int>(g.tris[i * 3]), static_cast<int>(g.tris[i * 3 + 1]),
                                             static_cast<int>(g.tris[i * 3 + 2])});
                t.bmin[1] = std::min(t.bmin[1], tlo.y);
                t.bmax[1] = std::max(t.bmax[1], thi.y);
            }
    }

    // Each tile's name: what it is made of; its layers from the cache, or cooked.
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        TileInput& t = tiles[i];
        if (t.tris.empty()) continue;
        ++out.tiles;
        // Heights on the cell-height grid, so neighbouring tiles voxelize alike.
        t.bmin[1] = std::floor(t.bmin[1] / s.cellHeight) * s.cellHeight - s.cellHeight;
        t.bmax[1] = std::ceil(t.bmax[1] / s.cellHeight) * s.cellHeight + s.agentHeight;
        ContentHasher h;
        h.text("ohao.navtile");
        h.value(kTileFormat);
        h.value(static_cast<uint32_t>(DT_TILECACHE_VERSION));
        h.value(s.cellSize); h.value(s.cellHeight); h.value(s.tileCells); h.value(s.agentRadius);
        h.value(s.agentHeight); h.value(s.agentClimb); h.value(s.slopeDeg);
        h.value(t.tx); h.value(t.ty);
        h.bytes(t.bmin, sizeof t.bmin);
        h.bytes(t.bmax, sizeof t.bmax);
        for (const int k : t.tris) h.value(g.verts[static_cast<std::size_t>(k)]);
        t.key = h.finish();
        bool have = false;
        if (!cacheDir.empty()) {
            const auto file = cacheDir / (t.key.hex() + ".navtile");
            std::error_code ec;
            if (std::filesystem::exists(file, ec))
                if (const auto mapped = cooked::Mapping::open(file)) have = readTileFile(mapped.value()->bytes(), t);
        }
        if (have) ++out.cached;
        else todo.push_back(i);
    }
    const auto tc = std::chrono::steady_clock::now();
    std::vector<uint8_t> ok(todo.size(), 1);
    jobs::parallelFor(0, todo.size(), 1, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t k = lo; k < hi; ++k) {
            TileInput& t = tiles[todo[k]];
            t.cookedNow = true;
            ok[k] = cookTile(s, g.verts, t) ? 1 : 0;
        }
    }, "navigation.cook", jobs::Priority::Background);
    out.cookMs = todo.empty() ? 0.0 : msSince(tc);
    for (std::size_t k = 0; k < todo.size(); ++k) {
        TileInput& t = tiles[todo[k]];
        ++out.cooked;
        if (!ok[k]) {
            out.warnings.push_back("tile (" + std::to_string(t.tx) + ", " + std::to_string(t.ty) + ") did not cook");
            t.layers.clear();
            continue;
        }
        if (cacheDir.empty()) continue;
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        if (const auto written = cooked::writeFile(cacheDir / (t.key.hex() + ".navtile"), tileFile(t)); !written)
            OHAO_LOG(Warn, asset, "a navmesh tile was not kept: {}", written.error());
    }

    // The tile cache and the navmesh over the grid.
    const int maxTiles = tw * th * kMaxLayers;
    dtTileCacheParams tcp{};
    tcp.orig[0] = origX;
    tcp.orig[1] = lo.y;
    tcp.orig[2] = origZ;
    tcp.cs = s.cellSize;
    tcp.ch = s.cellHeight;
    tcp.width = s.tileCells;
    tcp.height = s.tileCells;
    tcp.walkableHeight = s.agentHeight;
    tcp.walkableRadius = s.agentRadius;
    tcp.walkableClimb = s.agentClimb;
    tcp.maxSimplificationError = 1.3f;
    tcp.maxTiles = maxTiles;
    tcp.maxObstacles = 1024;
    m->process.set(g.links);
    m->cache = dtAllocTileCache();
    m->navmesh = dtAllocNavMesh();
    if (!m->cache || !m->navmesh || dtStatusFailed(m->cache->init(&tcp, &m->alloc, &m->comp, &m->process))) {
        m->clear();
        out.warnings.push_back("the tile cache could not be made");
        return false;
    }
    const int tileBits = std::min(static_cast<int>(dtIlog2(dtNextPow2(static_cast<unsigned int>(maxTiles)))), 14);
    dtNavMeshParams np{};
    np.orig[0] = origX;
    np.orig[1] = lo.y;
    np.orig[2] = origZ;
    np.tileWidth = ts;
    np.tileHeight = ts;
    np.maxTiles = 1 << tileBits;
    np.maxPolys = 1 << (22 - tileBits);
    if (dtStatusFailed(m->navmesh->init(&np))) {
        m->clear();
        out.warnings.push_back("the navmesh could not be made");
        return false;
    }
    for (TileInput& t : tiles)
        for (auto& layer : t.layers) {
            auto* data = static_cast<unsigned char*>(dtAlloc(static_cast<int>(layer.size()), DT_ALLOC_PERM));
            std::memcpy(data, layer.data(), layer.size());
            if (dtStatusFailed(m->cache->addTile(data, static_cast<int>(layer.size()), DT_COMPRESSEDTILE_FREE_DATA, nullptr)))
                dtFree(data);
            else ++out.layers;
        }
    for (const TileInput& t : tiles)
        if (!t.layers.empty()) m->cache->buildNavMeshTilesAt(t.tx, t.ty, m->navmesh);
    out.polys = polyCount();
    out.loadMs = msSince(t0);
    return true;
}

void Mesh::setObstacles(std::span<const Box> boxes) {
    if (!m->cache) return;
    const float r = m->settings.agentRadius;
    std::map<uint64_t, const Box*> want;
    for (const Box& b : boxes) want[b.id] = &b;
    const auto same = [](const Box& a, const Box& b) {
        return a.centre == b.centre && a.half == b.half && a.yaw == b.yaw;
    };
    bool changed = false;
    // The tile cache takes 64 requests between updates.
    const auto request = [&](auto&& fn) {
        for (int guard = 0; guard < 1000; ++guard) {
            const dtStatus st = fn();
            if (!dtStatusDetail(st, DT_BUFFER_TOO_SMALL)) return;
            bool upToDate = false;
            m->cache->update(0.0f, m->navmesh, &upToDate);
        }
    };
    for (auto it = m->obstacles.begin(); it != m->obstacles.end();) {
        const auto w = want.find(it->first);
        if (w != want.end() && same(*w->second, it->second.box)) {
            ++it;
            continue;
        }
        const dtObstacleRef ref = it->second.ref;
        request([&] { return m->cache->removeObstacle(ref); });
        it = m->obstacles.erase(it);
        changed = true;
    }
    for (const auto& [id, b] : want) {
        if (m->obstacles.contains(id)) continue;
        const float centre[3] = {b->centre.x, b->centre.y, b->centre.z};
        // Grown by the agent's radius: the layers are eroded by it, so the agents' centres keep it off the box.
        const float half[3] = {b->half.x + r, b->half.y, b->half.z + r};
        dtObstacleRef ref = 0;
        request([&] { return m->cache->addBoxObstacle(centre, half, b->yaw, &ref); });
        m->obstacles[id] = Impl::Placed{*b, ref};
        changed = true;
    }
    if (!changed) return;
    m->settle();
    ++m->epoch;
}

// ---- queries -----------------------------------------------------------------------------------------------------

struct Query::Impl {
    const Mesh& mesh;
    dtNavMeshQuery* query{dtAllocNavMeshQuery()};
    dtObstacleAvoidanceQuery* avoid{dtAllocObstacleAvoidanceQuery()};
    const dtNavMesh* bound{nullptr};
    dtQueryFilter filter;
    std::vector<dtPolyRef> polys = std::vector<dtPolyRef>(256);

    explicit Impl(const Mesh& m) : mesh(m) {
        filter.setIncludeFlags(kWalk);
        filter.setExcludeFlags(0);
        avoid->init(16, 0);
    }
    ~Impl() {
        dtFreeNavMeshQuery(query);
        dtFreeObstacleAvoidanceQuery(avoid);
    }
    bool bind() {
        const dtNavMesh* nm = mesh.impl().navmesh;
        if (!nm) return false;
        if (nm != bound) {
            if (dtStatusFailed(query->init(nm, 2048))) return false;
            bound = nm;
        }
        return true;
    }
};

Query::Query(const Mesh& mesh) : m(std::make_unique<Impl>(mesh)) {}
Query::~Query() = default;

std::optional<glm::vec3> Query::nearest(glm::vec3 p, glm::vec3 extents) {
    if (!m->bind()) return std::nullopt;
    dtPolyRef ref = 0;
    float out[3];
    if (dtStatusFailed(m->query->findNearestPoly(&p.x, &extents.x, &m->filter, &ref, out)) || !ref) return std::nullopt;
    return glm::vec3(out[0], out[1], out[2]);
}

PathResult Query::path(glm::vec3 from, glm::vec3 to, uint32_t maxCorners, glm::vec3 extents) {
    PathResult r;
    if (!m->bind()) {
        r.why = "no navmesh";
        return r;
    }
    maxCorners = std::clamp<uint32_t>(maxCorners, 2u, 256u);
    dtPolyRef start = 0, goal = 0;
    float sp[3], gp[3];
    m->query->findNearestPoly(&from.x, &extents.x, &m->filter, &start, sp);
    m->query->findNearestPoly(&to.x, &extents.x, &m->filter, &goal, gp);
    if (!start) {
        r.why = "the start is off the navmesh";
        return r;
    }
    if (!goal) {
        r.why = "the goal is off the navmesh";
        return r;
    }
    int n = 0;
    const dtStatus st = m->query->findPath(start, goal, sp, gp, &m->filter, m->polys.data(), &n,
                                           static_cast<int>(m->polys.size()));
    if (dtStatusFailed(st) || n == 0) {
        r.why = "no corridor";
        return r;
    }
    r.status = (m->polys[static_cast<std::size_t>(n - 1)] != goal || dtStatusDetail(st, DT_PARTIAL_RESULT))
                   ? PathResult::Status::Partial
                   : PathResult::Status::Found;
    float end[3] = {gp[0], gp[1], gp[2]};
    if (r.status == PathResult::Status::Partial)
        m->query->closestPointOnPoly(m->polys[static_cast<std::size_t>(n - 1)], gp, end, nullptr);
    std::vector<float> straight(maxCorners * 3);
    std::vector<unsigned char> flags(maxCorners);
    std::vector<dtPolyRef> refs(maxCorners);
    int corners = 0;
    m->query->findStraightPath(sp, end, m->polys.data(), n, straight.data(), flags.data(), refs.data(), &corners,
                               static_cast<int>(maxCorners), 0);
    r.polys = static_cast<uint32_t>(n);
    for (int i = 0; i < corners; ++i) {
        const glm::vec3 c(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
        if (!r.corners.empty()) r.length += glm::length(c - r.corners.back());
        r.corners.push_back(c);
    }
    if (r.corners.empty()) {
        r.status = PathResult::Status::NoPath;
        r.why = "no corners";
    }
    return r;
}

glm::vec3 Query::avoid(glm::vec3 pos, float radius, float maxSpeed, glm::vec3 vel, glm::vec3 desired,
                       std::span<const Neighbour> neighbours) {
    if (neighbours.empty()) return desired;
    dtObstacleAvoidanceQuery& q = *m->avoid;
    q.reset();
    for (std::size_t i = 0; i < neighbours.size() && i < 16; ++i)
        q.addCircle(&neighbours[i].pos.x, neighbours[i].radius, &neighbours[i].vel.x, &neighbours[i].vel.x);
    // DetourCrowd's "good" quality: an adaptive pattern of 7 directions, 2 rings, 3 refinements.
    dtObstacleAvoidanceParams p{};
    p.velBias = 0.4f;
    p.weightDesVel = 2.0f;
    p.weightCurVel = 0.75f;
    p.weightSide = 0.75f;
    p.weightToi = 2.5f;
    p.horizTime = 2.5f;
    p.gridSize = 33;
    p.adaptiveDivs = 7;
    p.adaptiveRings = 2;
    p.adaptiveDepth = 3;
    glm::vec3 out(0.0f);
    q.sampleVelocityAdaptive(&pos.x, radius, maxSpeed, &vel.x, &desired.x, &out.x, &p);
    return out;
}

} // namespace ohao::nav
