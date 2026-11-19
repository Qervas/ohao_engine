#pragma once
// Navigation (docs/specs/2026-11-18-tidewall-design.md, 3.1): a navmesh of the World's static collision on
// Recast/Detour, the paths agents follow on it, and the obstacles that block it while present.
//
//   - The navmesh is built from every static, non-trigger Collider (box, mesh, hull; a sphere or capsule as its
//     bounds) but those of entities with a NavObstacle, plus the NavLinks, in tiles of 19.2 m: each tile's layers are
//     cooked once and kept in the cooked cache under the hash of the triangles that make it and the settings, so a
//     layout edit cooks only its tiles. It is built when first needed (an active NavAgent, a method) and again after a
//     collider or link changes; a World without agents never builds one.
//   - Obstacles: at the start of the `navigation` step the set of active NavObstacles is applied to the tile cache to
//     completion, so the navmesh at step S is a function of the state at S.
//   - The version is a hash of the navmesh's input (the baked geometry and the obstacle set): a function of saved
//     state, so nothing of the navmesh needs a snapshot. A path planned on another version is planned again.
//   - The `navigation` simulate system (before `physics`): each active NavAgent, in id order, in chunks on the job pool,
//     each chunk with its own Detour query: a path planned when it has none, on a new version, and every replanSeconds
//     (staggered by id); its corners followed; the velocity steered around the other agents (Detour's sampled-velocity
//     avoidance, from their positions and velocities as the step began); the character's MoveIntent written. The
//     physics step moves the character: it is the one authority over where a character is.
#include "ecs/world.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ohao::ecs {

struct NavInfo {
    bool built{false};
    uint64_t version{0};          // the hash of the navmesh's input; 0 without a navmesh
    uint32_t tiles{0}, cooked{0}, cached{0}, layers{0}, polys{0}, links{0}, obstacles{0};
    uint32_t triangles{0};        // of the static collision it was built from
    double cookMs{0.0}, buildMs{0.0};   // the last build: the tiles cooked now; the whole build (gather to tiles)
    double obstaclesMs{0.0};      // the last change of the obstacle set, applied to completion
    float cellSize{0.0f}, cellHeight{0.0f}, tileSize{0.0f}, agentRadius{0.0f}, agentHeight{0.0f}, agentClimb{0.0f},
        slopeDeg{0.0f};
    uint32_t agents{0};           // entities with a NavAgent
    std::vector<std::string> warnings;
};

struct NavRoute {
    enum class Status : uint8_t { Found, Partial, NoPath } status{Status::NoPath};
    std::vector<glm::vec3> corners;
    float length{0.0f};
    uint32_t polys{0};
    std::string why;   // NoPath: the reason
};

// Where the last `navigation` step's time went.
struct NavStepTiming {
    double totalMs{0.0};
    double obstaclesMs{0.0};   // the obstacle set applied (0 when it did not change)
    double agentsMs{0.0};      // the agents planned, followed and steered
    uint32_t agents{0}, planned{0};
};

// The navmesh's state, building it first when it is out of date. nullopt without the navigation module.
[[nodiscard]] std::optional<NavInfo> navInfo(World& world);
// The shortest path between two points (each snapped to the navmesh within 2 m across and 4 m up and down).
[[nodiscard]] std::optional<NavRoute> navPath(World& world, glm::vec3 from, glm::vec3 to, uint32_t maxCorners = 32);
// The nearest navmesh point within `extents` (half sizes) of p.
[[nodiscard]] std::optional<glm::vec3> navNearest(World& world, glm::vec3 p, glm::vec3 extents);
// What nav.show draws, world space: every navmesh polygon's edges, every link, every active obstacle's box edges.
struct NavDrawing {
    std::vector<std::pair<glm::vec3, glm::vec3>> edges, links, obstacles;
};
[[nodiscard]] NavDrawing navDrawing(World& world);
[[nodiscard]] NavStepTiming navStepTiming(const World& world);

} // namespace ohao::ecs
