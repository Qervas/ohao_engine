// The physics module's registrations (physics/module.hpp), a part of types.cpp's: its event payloads.
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerPhysicsTypesEvents(reflect::TypeRegistry& r) {
    // ---- its events
    r.type<ContactEvent>("ContactEvent").event()
        .doc("physics.contact_begin and physics.contact_end: a body pair began or ended touching (sub-shape contacts "
             "collapsed; a removed body ends its pairs at once)")
        .field("a", &ContactEvent::a).entity().doc("The body with the lower id")
        .field("b", &ContactEvent::b).entity().doc("The other body")
        .field("point", &ContactEvent::point).doc("Where, world; an end repeats its begin's").unit("m")
        .field("normal", &ContactEvent::normal).doc("From a toward b, world").unitless()
        .field("speed", &ContactEvent::speed).doc("The relative normal speed at a begin; 0 at an end").unit("m/s");
    r.type<TriggerEvent>("TriggerEvent").event()
        .doc("physics.trigger_enter and physics.trigger_exit: a body entered or left a trigger (a Collider with trigger)")
        .field("trigger", &TriggerEvent::trigger).entity().doc("The trigger")
        .field("body", &TriggerEvent::body).entity().doc("The body")
        .field("point", &TriggerEvent::point).doc("Where, world; an exit repeats its enter's").unit("m")
        .field("normal", &TriggerEvent::normal).doc("From the trigger toward the body, world").unitless()
        .field("speed", &TriggerEvent::speed).doc("The relative normal speed at an enter; 0 at an exit").unit("m/s");
    r.type<FootEvent>("FootEvent").event()
        .doc("physics.footstep (every 0.75 m walked, 0.95 m sprinted, on the ground) and physics.land (touching the "
             "ground after a fall of more than 0.3 m): a player's")
        .field("player", &FootEvent::player).entity().doc("The player")
        .field("ground", &FootEvent::ground).entity().doc("The ground's entity; none when it has none")
        .field("point", &FootEvent::point).doc("The feet, world").unit("m")
        .field("normal", &FootEvent::normal).doc("The ground's normal, world").unitless()
        .field("speed", &FootEvent::speed).doc("A footstep: the horizontal speed; a landing: the downward speed").unit("m/s")
        .field("surface", &FootEvent::surface).doc("The ground's Collider.surface (default when it has none)");
    r.type<JointBreakEvent>("JointBreakEvent").event()
        .doc("physics.joint_break: a joint broke (its entity gets JointState)")
        .field("joint", &JointBreakEvent::joint).entity().doc("The Joint's entity")
        .field("other", &JointBreakEvent::other).entity().doc("The other body's entity; none: the world")
        .field("point", &JointBreakEvent::point).doc("Its anchor, world").unit("m");
}

} // namespace ohao::ecs
