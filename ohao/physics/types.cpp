// The physics module's registrations (physics/module.hpp): its enums, components and event payloads.
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

// The parts of its registrations, each its own unit (they compile in parallel), in this order.
void registerPhysicsTypesBodies(reflect::TypeRegistry& r);   // the colliders, bodies and joints (types_bodies.cpp)
void registerPhysicsTypesRuntime(reflect::TypeRegistry& r);   // its runtime components (types_runtime.cpp)
void registerPhysicsTypesEvents(reflect::TypeRegistry& r);   // its event payloads (types_events.cpp)

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
    registerPhysicsTypesBodies(r);
    registerPhysicsTypesRuntime(r);
    registerPhysicsTypesEvents(r);
}

} // namespace ohao::ecs
