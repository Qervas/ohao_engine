#pragma once
// The navigation module's components (navigation/module.hpp; docs/specs/2026-11-18-tidewall-design.md, 3.1): who
// walks the navmesh (NavAgent) and the path it follows (NavPath), what blocks it while present (NavObstacle) and the
// links across what the navmesh cannot walk (NavLink). A level's node takes NavObstacle and NavLink as hints: a
// capitalised key of its extras names the component (level/level_hints.hpp).
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

// A character (its PlayerController with input false) the navigation system walks to `goal`: it plans a path on the
// navmesh, follows its corners, steers around the other agents and writes the character's MoveIntent. Saved.
struct NavAgent {
    glm::vec3 goal{0.0f};      // m, world: where it walks (the nearest navmesh point to it)
    bool active{false};        // false: it stands (its MoveIntent is left as it is)
    float speed{1.0f};         // its MoveIntent's forward move, 0..1 of its walk speed
    float arrive{0.5f};        // m: within this of the goal (horizontally) it has arrived and stands
    float replanSeconds{0.5f}; // a path is planned again this often (staggered by entity id), and when the navmesh changes
    bool avoid{true};          // steer around the other agents (sampled velocities)
};

enum class NavPathStatus : uint8_t {
    None,      // not planned yet
    Found,     // a path to the goal
    Partial,   // the goal is not reachable: a path to the reachable point nearest it
    NoPath,    // the agent or the goal is off the navmesh
    Arrived,   // within `arrive` of the goal
};

// The path an agent follows, as the navigation system planned it: at most 32 world-space corners (never polygon
// references, so a snapshot or a save never names the navmesh's state), the corner it walks to, the step and the
// navmesh version it was planned on. Saved and hashed.
struct NavPath {
    std::vector<glm::vec3> corners;   // m, world; corners[0] is where the agent stood
    uint32_t next{0};                 // the corner it walks to
    NavPathStatus status{NavPathStatus::None};
    uint64_t plannedStep{0};          // the World's step it was planned in
    uint64_t version{0};              // the navmesh version it was planned on (navInfo().version)
    glm::vec3 goal{0.0f};             // m, world: the goal it was planned to
};

// While present and active, the entity blocks the navmesh as an oriented box (turned about +Y only): a door, a
// breakable wall. The entity's own collider is not baked into the navmesh; the box stands in for it. halfExtents
// zero: the box of the entity's mesh. Applied within the step that changes it.
struct NavObstacle {
    glm::vec3 halfExtents{0.0f};   // m, the entity's frame (its scale applies); zero: the mesh's bounds
    bool active{true};
};

// A link the navmesh cannot walk: from the entity's position to `end` (in the entity's frame): a drop from a catwalk,
// a vault over a low wall. Baked into the navmesh as an off-mesh connection.
struct NavLink {
    glm::vec3 end{0.0f, 0.0f, -1.0f};   // m, the entity's frame
    bool bidirectional{false};          // false: one way, from the entity to `end`
    float radius{0.5f};                 // m: how near its ends a path may join it
};

} // namespace ohao::ecs

// The storages of these components, compiled once (navigation/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::NavAgent);
OHAO_EXTERN_STORAGE(ohao::ecs::NavLink);
OHAO_EXTERN_STORAGE(ohao::ecs::NavObstacle);
OHAO_EXTERN_STORAGE(ohao::ecs::NavPath);
