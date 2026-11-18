// The physics module's registrations (physics/module.hpp): its enums, components and event payloads.
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerPhysicsTypes(reflect::TypeRegistry& r) {
    r.enumeration<ColliderShape>("ColliderShape")
        .doc("A collider's shape, fitted to the entity's mesh unless a size is given.")
        .value("Box", ColliderShape::Box, "The mesh's bounding box")
        .value("Sphere", ColliderShape::Sphere, "Around the bounding box's centre, to the farthest vertex")
        .value("Capsule", ColliderShape::Capsule, "Along the bounding box's longest axis")
        .value("Convex", ColliderShape::Convex, "The convex hull of the mesh's vertices")
        .value("Mesh", ColliderShape::Mesh, "The mesh's triangles (static and kinematic bodies; a dynamic one is made convex)")
        .value("None", ColliderShape::None, "No collision");
    r.enumeration<Motion>("Motion")
        .doc("How a rigid body moves.")
        .value("Static", Motion::Static, "Never moves")
        .value("Dynamic", Motion::Dynamic, "Moved by the simulation; writes its entity's Transform each step")
        .value("Kinematic", Motion::Kinematic, "Moved by its transform; dynamic bodies collide with it");
    r.enumeration<JointType>("JointType")
        .doc("What a joint lets its two bodies do.")
        .value("Fixed", JointType::Fixed, "Nothing: welded as they were built")
        .value("Point", JointType::Point, "Turn freely about the anchor (ball and socket)")
        .value("Hinge", JointType::Hinge, "Turn about the axis through the anchor (a door)")
        .value("Slider", JointType::Slider, "Slide along the axis without turning")
        .value("SwingTwist", JointType::SwingTwist, "Swing within a cone about the axis and twist about it (a shoulder)")
        .value("Distance", JointType::Distance, "Keep the anchors within a distance (a rope, a chain link)");
    r.type<PlayerController>("PlayerController").component().host(makeComponentOps<PlayerController>())
        .doc("A first-person player: a capsule standing on the entity's position (its feet), turned about +Y. Once per "
             "fixed step, before bodies step, it walks from the named axes and actions (forward along its -Z, right "
             "along its +X), jumps, climbs steps and slopes, and pushes dynamic bodies. It spawns at the first "
             "SpawnPoint tagged \"player\"; triggers and rays see it.")
        .field("radius", &PlayerController::radius).doc("Capsule radius").unit("m").min(0.05).max(2.0)
        .field("height", &PlayerController::height).doc("Capsule height, feet to top").unit("m").min(0.2).max(5.0)
        .field("eyeHeight", &PlayerController::eyeHeight).doc("The eyes above the feet (the first-person camera)")
            .unit("m").min(0.0).max(5.0)
        .field("walkSpeed", &PlayerController::walkSpeed).doc("Speed at a move axis of 1").unit("m/s").min(0.0).max(50.0)
        .field("sprintSpeed", &PlayerController::sprintSpeed).doc("Speed while `sprint` is held").unit("m/s").min(0.0)
            .max(50.0)
        .field("acceleration", &PlayerController::acceleration).doc("How fast the horizontal velocity reaches the wanted one")
            .unit("m/s2").min(0.0).max(1000.0)
        .field("airControl", &PlayerController::airControl).doc("Fraction of the acceleration while in the air")
            .unitless().min(0.0).max(1.0)
        .field("jumpSpeed", &PlayerController::jumpSpeed).doc("Upward speed of a jump (4.9 m/s: 1.22 m high)").unit("m/s")
            .min(0.0).max(50.0)
        .field("maxSlopeDeg", &PlayerController::maxSlopeDeg).doc("Steepest walkable ground; steeper is a wall").unit("deg")
            .min(0.0).max(89.0)
        .field("stepHeight", &PlayerController::stepHeight).doc("Highest step climbed without jumping").unit("m").min(0.0)
            .max(2.0)
        .field("mass", &PlayerController::mass).doc("Mass pressing on what it stands on").unit("kg").min(1.0).max(10000.0)
        .field("pushForce", &PlayerController::pushForce).doc("The most force it pushes a dynamic body with").unit("N")
            .min(0.0).max(100000.0)
        .field("lookDegPerUnit", &PlayerController::lookDegPerUnit)
            .doc("Turn per unit of mouse movement, or of a look axis's value in a step").unit("deg").min(0.0).max(10.0)
        .field("moveForward", &PlayerController::moveForward).doc("Axis walking along the player's -Z")
        .field("moveRight", &PlayerController::moveRight).doc("Axis walking along the player's +X")
        .field("jump", &PlayerController::jump).doc("Action that jumps when pressed on the ground")
        .field("sprint", &PlayerController::sprint).doc("Action that, while held, walks at sprintSpeed")
        .field("lookX", &PlayerController::lookX).doc("Axis turning the player (yaw), added to the mouse's x")
        .field("lookY", &PlayerController::lookY).doc("Axis tilting the view (pitch), added to the mouse's y")
        .field("spawn", &PlayerController::spawn)
            .doc("The SpawnPoint tag it spawns at: the k-th character with a tag (creation order) takes the k-th "
                 "spawn point with it; empty: it starts where it stands")
            .maxLength(32)
        .field("input", &PlayerController::input)
            .doc("Moved by the world's input (a player); false: by its MoveIntent (an AI or an agent)")
        .field("stream", &PlayerController::stream)
            .doc("Its input stream: one per player; a device's events go to 0, an agent's to the player's it names")
            .unitless().min(0).max(7).omitDefault();
    r.type<MoveIntent>("MoveIntent").component().host(makeComponentOps<MoveIntent>())
        .doc("The move a character without input (PlayerController.input false) makes each step, written by the "
             "enemy AI or an agent. Absent: it stands still, facing as it faces.")
        .field("move", &MoveIntent::move).doc("x right, y forward, in its own frame; each within -1..1").unitless()
            .min(-1.0).max(1.0)
        .field("yawDeg", &MoveIntent::yawDeg).doc("The facing to take, about +Y from -Z").unit("deg").min(-360.0)
            .max(360.0)
        .field("pitchDeg", &MoveIntent::pitchDeg).doc("The look up (positive) or down").unit("deg").min(-89.0).max(89.0)
        .field("jump", &MoveIntent::jump).doc("Jump (when on the ground)")
        .field("sprint", &MoveIntent::sprint).doc("Walk at sprintSpeed");
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
    r.type<ColliderPart>("ColliderPart").component().runtime().host(makeComponentOps<ColliderPart>())
        .doc("This entity's collider is part of another entity's body (a level's UCX_ hulls): one compound body.")
        .field("body", &ColliderPart::body).doc("The entity whose body it is part of");
    r.type<CollisionMesh>("CollisionMesh").component().runtime().host(makeComponentOps<CollisionMesh>())
        .doc("The mesh a level node's collider is fitted to when the node is not drawn (vis 0, UCX_ hulls) and so has "
             "no MeshSource. Set by the level system.")
        .field("source", &CollisionMesh::source).doc("The glTF mesh").asset("collision model");
    r.type<PhysicsBody>("PhysicsBody").component().runtime().host(makeComponentOps<PhysicsBody>())
        .doc("The entity's body in the physics backend. Owned by the physics system.")
        .field("handle", &PhysicsBody::handle).doc("Body handle; 4294967295 means none").unitless();
    r.type<PhysicsMoved>("PhysicsMoved").component().runtime()
        .doc("Tag: an entity with a body moved; the physics system moves the body this frame.");
    r.type<PlayerState>("PlayerState").component().runtime().host(makeComponentOps<PlayerState>())
        .doc("The player as the physics system left it after the last step. Owned by the physics system.")
        .field("velocity", &PlayerState::velocity).doc("World velocity").unit("m/s")
        .field("onGround", &PlayerState::onGround).doc("Standing on walkable ground")
        .field("yawDeg", &PlayerState::yawDeg).doc("Facing about +Y from -Z, counter-clockwise seen from above").unit("deg")
        .field("pitchDeg", &PlayerState::pitchDeg).doc("Looking up (positive) or down, within 89 degrees").unit("deg")
        .field("character", &PlayerState::character).doc("Character handle; 4294967295 means none").unitless();
    r.type<BodyMotion>("BodyMotion").component().runtime().snapshotted().host(makeComponentOps<BodyMotion>())
        .doc("The motion a dynamic body or a player carries into the next step. Written by the physics system; "
             "never saved or hashed, but held by snapshots (a restore whose bodies were rebuilt since puts it back).")
        .field("linear", &BodyMotion::linear).doc("Linear velocity, world (a player's velocity)").unit("m/s")
        .field("angular", &BodyMotion::angular).doc("Angular velocity, world").unit("rad/s")
        .field("asleep", &BodyMotion::asleep).doc("The body sleeps; its velocities are zero");
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
