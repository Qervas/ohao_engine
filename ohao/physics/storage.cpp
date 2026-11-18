// The physics module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "physics/components.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::BodyMotion);
OHAO_STORAGE_INSTANCE(ohao::ecs::Collider);
OHAO_STORAGE_INSTANCE(ohao::ecs::ColliderPart);
OHAO_STORAGE_INSTANCE(ohao::ecs::CollisionMesh);
OHAO_STORAGE_INSTANCE(ohao::ecs::Joint);
OHAO_STORAGE_INSTANCE(ohao::ecs::JointState);
OHAO_STORAGE_INSTANCE(ohao::ecs::MoveIntent);
OHAO_STORAGE_INSTANCE(ohao::ecs::PhysicsBody);
OHAO_STORAGE_INSTANCE(ohao::ecs::PhysicsMoved);
OHAO_STORAGE_INSTANCE(ohao::ecs::PlayerController);
OHAO_STORAGE_INSTANCE(ohao::ecs::PlayerState);
OHAO_STORAGE_INSTANCE(ohao::ecs::RigidBody);
