#pragma once
// The physics module's events (physics/module.hpp): the channels physics emits into the World's log (ecs/events.hpp)
// and their payloads.
#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace ohao::ecs {

// Physics' events (spec "Events" and "Footsteps"), each a channel of the World's log. Entities are held by
// EntityId: they may be gone by the time an event is read.
namespace physics_events {
inline constexpr const char* kContactBegin = "physics.contact_begin";   // ContactEvent
inline constexpr const char* kContactEnd = "physics.contact_end";       // ContactEvent
inline constexpr const char* kTriggerEnter = "physics.trigger_enter";   // TriggerEvent
inline constexpr const char* kTriggerExit = "physics.trigger_exit";     // TriggerEvent
inline constexpr const char* kFootstep = "physics.footstep";            // FootEvent
inline constexpr const char* kLand = "physics.land";                    // FootEvent
inline constexpr const char* kJointBreak = "physics.joint_break";       // JointBreakEvent
} // namespace physics_events

// A body pair began or ended touching.
struct ContactEvent {
    uint64_t a{0}, b{0};        // a has the lower id
    glm::vec3 point{0.0f};      // world; an end repeats its begin's
    glm::vec3 normal{0.0f};     // world, from a toward b
    float speed{0.0f};          // the relative normal speed at a begin (m/s); 0 at an end
};
// A body entered or left a trigger (a Collider with trigger).
struct TriggerEvent {
    uint64_t trigger{0}, body{0};
    glm::vec3 point{0.0f}, normal{0.0f};   // as a contact's; the normal from the trigger toward the body
    float speed{0.0f};                     // at an enter (m/s); 0 at an exit
};
// A player's footstep (every 0.75 m walked, 0.95 m sprinted, on the ground) or landing (touching the ground
// after a fall of more than 0.3 m).
struct FootEvent {
    uint64_t player{0};
    uint64_t ground{0};          // the ground's entity (0: none)
    glm::vec3 point{0.0f};       // the feet
    glm::vec3 normal{0.0f};      // the ground's
    float speed{0.0f};           // a footstep: the horizontal speed; a landing: the downward speed (m/s)
    std::string surface;         // the ground's Collider.surface ("default" if none)
};
// A joint broke.
struct JointBreakEvent {
    uint64_t joint{0};           // the Joint's entity
    uint64_t other{0};           // the other body's entity (0: the world)
    glm::vec3 point{0.0f};       // its anchor, world
};

} // namespace ohao::ecs
