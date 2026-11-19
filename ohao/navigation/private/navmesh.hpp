#pragma once
// The navigation module's core (no World): Recast/Detour behind a few types (docs/specs/2026-11-18-tidewall-design.md,
// 3.1). A Mesh is built from world-space triangles and links, one dtTileCache layer set per tile, each tile cooked once
// and kept in the cooked cache under the hash of what it is made of (the tile's triangles, links and the settings), so a
// layout edit cooks only its tiles; tiles cook in parallel on the job pool (each tile's bytes depend on its input
// alone). Obstacles (oriented boxes) are applied to the tile cache, which rebuilds the tiles they touch, to completion.
// A Query is one thread's: Detour's node pool and the avoidance query's state are its own, cleared per call, so its
// results are a function of the mesh and the arguments.
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ohao::nav {

// The one agent class's settings and the voxels (cells) the navmesh is built of.
struct Settings {
    float cellSize{0.12f};      // m, horizontal
    float cellHeight{0.1f};     // m, vertical
    int tileCells{160};         // cells a tile's side (19.2 m)
    float agentRadius{0.35f};   // m
    float agentHeight{1.8f};    // m
    float agentClimb{0.35f};    // m, the highest step
    float slopeDeg{45.0f};      // steeper ground is not walkable
    [[nodiscard]] float tileSize() const noexcept { return cellSize * static_cast<float>(tileCells); }
};

// An off-mesh connection: from a to b, one way unless bidirectional.
struct Link {
    glm::vec3 a{0.0f}, b{0.0f};
    float radius{0.5f};
    bool bidirectional{false};
    uint32_t id{0};
};

// What a navmesh is built from: world-space triangles (counter-clockwise seen from their front) and links.
struct Geometry {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> tris;   // three vertex indices each
    std::vector<Link> links;
};

// An obstacle: a box turned about +Y by yaw radians, in world space. id orders the set.
struct Box {
    uint64_t id{0};
    glm::vec3 centre{0.0f}, half{0.0f};
    float yaw{0.0f};
};

struct BuildReport {
    uint32_t tiles{0};       // tiles of the grid with geometry
    uint32_t cooked{0};      // of those, cooked now
    uint32_t cached{0};      // read from the cooked cache
    uint32_t layers{0};      // tile cache layers
    uint32_t polys{0};       // navmesh polygons with no obstacle
    double cookMs{0.0};      // the cook of the tiles not in the cache (in parallel)
    double loadMs{0.0};      // the whole build: keys, cache reads, the cook, the navmesh's tiles
    std::vector<std::string> warnings;
};

class Mesh {
public:
    Mesh();
    ~Mesh();
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    // Replaces the navmesh with one built from `g` (no obstacles). `cacheDir` empty: nothing is kept or read.
    bool build(const Geometry& g, const Settings& s, const std::filesystem::path& cacheDir, BuildReport& out);
    // Sets the obstacles to exactly `boxes` (each id once): those gone are removed, the new and the changed added, and
    // the tile cache rebuilds the tiles they touch before this returns.
    void setObstacles(std::span<const Box> boxes);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const Settings& settings() const noexcept;
    // Changes with each build and obstacle change: a Query re-binds when it differs.
    [[nodiscard]] uint64_t epoch() const noexcept;
    [[nodiscard]] uint32_t polyCount() const;
    [[nodiscard]] uint32_t obstacleCount() const noexcept;
    // Every polygon's outline and each link's ends (world): what nav.show draws.
    void outlines(std::vector<std::pair<glm::vec3, glm::vec3>>& edges,
                  std::vector<std::pair<glm::vec3, glm::vec3>>& links) const;

    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *m; }

private:
    std::unique_ptr<Impl> m;
};

struct PathResult {
    enum class Status : uint8_t { Found, Partial, NoPath } status{Status::NoPath};
    std::vector<glm::vec3> corners;   // from the start (on the navmesh) to the goal, at most maxCorners
    float length{0.0f};               // m, along the corners
    uint32_t polys{0};                // polygons in the corridor
    std::string why;                  // NoPath: the reason
};

// Another agent near the one steering: where it is, how fast it moves, how wide it is.
struct Neighbour {
    glm::vec3 pos{0.0f}, vel{0.0f};
    float radius{0.35f};
};

class Query {
public:
    explicit Query(const Mesh& mesh);
    ~Query();
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    // The shortest path from `from` to `to` (each snapped to the nearest polygon within `extents`), string-pulled to
    // at most maxCorners corners.
    [[nodiscard]] PathResult path(glm::vec3 from, glm::vec3 to, uint32_t maxCorners = 32,
                                  glm::vec3 extents = glm::vec3(2.0f, 4.0f, 2.0f));
    // The nearest navmesh point within `extents` of p.
    [[nodiscard]] std::optional<glm::vec3> nearest(glm::vec3 p, glm::vec3 extents = glm::vec3(2.0f, 4.0f, 2.0f));
    // A velocity near `desired` that keeps clear of the neighbours (Detour's sampled-velocity avoidance, adaptive).
    [[nodiscard]] glm::vec3 avoid(glm::vec3 pos, float radius, float maxSpeed, glm::vec3 vel, glm::vec3 desired,
                                  std::span<const Neighbour> neighbours);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::nav
