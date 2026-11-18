// The physics module's registrations (physics/module.hpp), a part of types.cpp's: the colliders, bodies and joints.
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerPhysicsTypesBodies(reflect::TypeRegistry& r) {
    r.type<Collider>("Collider").component().host(makeComponentOps<Collider>())
        .doc("The entity collides: the physics system builds a body for it (static unless it has a RigidBody). A zero "
             "size fits the shape to the entity's mesh, in its frame and at its world scale.")
        .field("shape", &Collider::shape).doc("The shape")
        .field("size", &Collider::size)
            .doc("Zero: fit to the mesh. Box: half extents; Sphere: x is the radius; Capsule: x the radius, y the half "
                 "height of its straight part")
            .unit("m").min(0.0)
        .field("offset", &Collider::offset).doc("The shape's centre in the entity's frame, with an explicit size").unit("m")
        .field("trigger", &Collider::trigger)
            .doc("A trigger volume: reports bodies entering and leaving it (trigger_enter, trigger_exit) and blocks "
                 "nothing")
        .field("surface", &Collider::surface)
            .doc("What it is made of, for footsteps on it (concrete, metal, wood, ...); empty is \"default\"")
            .maxLength(32).omitDefault();
    r.type<RigidBody>("RigidBody").component().host(makeComponentOps<RigidBody>())
        .doc("How the entity's body moves. Dynamic bodies are simulated at the world's fixed step and move their "
             "entity; static and kinematic ones follow it.")
        .field("motion", &RigidBody::motion).doc("Static, Dynamic or Kinematic")
        .field("mass", &RigidBody::mass).doc("Mass of a dynamic body").unit("kg").min(0.001)
        .field("friction", &RigidBody::friction).doc("Surface friction").unitless().min(0.0).max(1.0)
        .field("restitution", &RigidBody::restitution).doc("Bounciness: 0 none, 1 elastic").unitless().min(0.0).max(1.0);
    r.type<Joint>("Joint").component().host(makeComponentOps<Joint>())
        .doc("A joint between the body this entity is part of (its own, else its nearest ancestor's) and another's, or "
             "the world. Anchor and axis are in this entity's frame; the other side is the same world point and axis "
             "when the joint is built, except a Distance joint's, which is the other entity's origin. Several joints "
             "on one body: child entities with a Joint each.")
        .field("type", &Joint::type).doc("What the joint lets the bodies do")
        .field("other", &Joint::other).doc("The other body's entity (its own body, else its nearest ancestor's); null: the world")
        .field("anchor", &Joint::anchor).doc("The joint's point in this entity's frame").unit("m")
        .field("axis", &Joint::axis).doc("Hinge axis, slider axis or swing-twist's twist axis, in this entity's frame").unitless()
        .field("limitMin", &Joint::limitMin)
            .doc("Hinge: degrees this entity turns about the axis (right-handed) from the built pose, relative to the "
                 "other; slider: m along the axis; swing-twist: twist degrees; distance: m. With limitMax 0 too: free "
                 "(hinge, slider), the built distance (distance)")
            .unitless().omitDefault()
        .field("limitMax", &Joint::limitMax).doc("The limit's other end, as limitMin").unitless().omitDefault()
        .field("swingDeg", &Joint::swingDeg).doc("Swing-twist: the swing cone's half angle about the axis")
            .unit("deg").angle().min(0.0).max(180.0).omitDefault()
        .field("springHz", &Joint::springHz).doc("Distance: a soft range's frequency; 0 is rigid").unit("Hz").min(0.0).omitDefault()
        .field("springDamping", &Joint::springDamping).doc("Distance: the soft range's damping ratio").unitless().min(0.0).omitDefault()
        .field("breakForce", &Joint::breakForce).doc("The impulse in one step that breaks it; 0 never").unit("N s").min(0.0).omitDefault()
        .field("breakTorque", &Joint::breakTorque).doc("The angular impulse in one step that breaks it; 0 never")
            .unit("N m s").min(0.0).omitDefault();
    r.type<JointState>("JointState").component().host(makeComponentOps<JointState>())
        .doc("The joint broke: its constraint stays built and does nothing. The physics step adds it; a restore from "
             "before the break makes the joint whole.")
        .field("broken", &JointState::broken).doc("The joint is broken; false makes it whole again");
}

} // namespace ohao::ecs
