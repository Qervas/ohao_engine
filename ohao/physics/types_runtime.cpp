// The physics module's registrations (physics/module.hpp), a part of types.cpp's: its runtime components.
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerPhysicsTypesRuntime(reflect::TypeRegistry& r) {
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
}

} // namespace ohao::ecs
