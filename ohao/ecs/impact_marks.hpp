#pragma once
// Impact marks (sub-project 7 spec, "Presentation"): a dark 8 cm quad where each round hit, 2 mm
// off the surface along its normal, facing it. A pool of 64: the 65th mark takes the oldest's
// slot. A mark is attached to what was hit and follows it (an enemy walks, falls, lies down with
// its marks); a round that hit a character is marked on the character's mesh box (a BoxPrim child)
// along the round's path, or not at all if the path misses the box; none on the input-driven
// player (it would hang in front of its own eyes). A restart clears them. Runtime only: nothing
// in the registry, never saved or hashed. installCombat adds the `impact_marks` simulate system
// after `combat`, which takes the step's hits against the step's poses (so every host, its
// renderer made early or late, has the same marks); the render side (installImpactMarks) draws
// them as RenderScene instances of its own.
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace ohao::render {
class RenderScene;
}

namespace ohao::ecs {

class World;

struct ImpactMark {
    entt::entity on{entt::null};   // what it is attached to (null: the world)
    glm::mat4 local{1.0f};         // the quad's pose in `on`'s frame (the world's for null)
    glm::mat4 world{1.0f};         // the quad's pose now (follow)
    uint64_t serial{0};            // 1 = the first mark ever made
};

class ImpactMarks {
public:
    static constexpr std::size_t kCapacity = 64;
    static constexpr float kSize = 0.08f;    // m, the quad's side
    static constexpr float kLift = 0.002f;   // m off the surface

    // Reads the combat log since the last call (the first call from the oldest event it holds, so a
    // host that starts drawing late sees the same marks): a mark for each hit, all cleared by a
    // restart.
    void observe(const World& world);
    // A mark on `on` (null: the world) at `point` facing `normal`.
    void add(const World& world, entt::entity on, glm::vec3 point, glm::vec3 normal);
    void clear();
    // Each mark's world pose from its entity's (a mark whose entity is gone stays where it was).
    void follow(const World& world);
    // The pool by slot.
    [[nodiscard]] std::span<const ImpactMark> marks() const noexcept { return m_marks; }
    [[nodiscard]] uint64_t made() const noexcept { return m_serial; }

private:
    std::vector<ImpactMark> m_marks;
    std::size_t m_next{0};          // the slot the next mark takes once the pool is full
    uint64_t m_serial{0};
    uint64_t m_cursor{0};           // 0: the first observe reads the log from its oldest event (a host made late sees the same marks)
    std::unordered_map<uint64_t, glm::vec3> m_lastShot;   // shooter id -> its last round's direction
};

// Draws the World's marks (made on first use) as instances of one quad mesh in `scene`: a sync
// system after the render extract. The marks themselves: impactMarksOf.
void installImpactMarks(World& world, render::RenderScene& scene);
[[nodiscard]] ImpactMarks& impactMarksOf(World& world);

} // namespace ohao::ecs
