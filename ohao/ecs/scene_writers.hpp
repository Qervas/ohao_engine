#pragma once
// Per-type scene writers: each authoring component's fields written straight into a
// reflect::LayoutTree, with its keys as literals and no reflection walk. They write
// exactly what the generic writer (reflect::writeStruct) writes for the registered fields,
// in registration order; scene_tests t21 checks every one against it on random values.
//
// A field equal to `omit`'s is left out (the canonical form; omit is the type's default),
// and nothing is left out when omit is null. Values nested in variants and optionals are
// written in full. makeComponentOps<T>() picks up the overload for T when there is one.
#include "ecs/components.hpp"

namespace ohao::reflect { class LayoutTree; }

namespace ohao::ecs {

class World;

struct FieldSink {
    reflect::LayoutTree& tree;
    const World& world;   // entity references are written as the target's EntityId
};

void writeFields(const Name& v, const Name* omit, FieldSink& out);
void writeFields(const Transform& v, const Transform* omit, FieldSink& out);
void writeFields(const Parent& v, const Parent* omit, FieldSink& out);
void writeFields(const Light& v, const Light* omit, FieldSink& out);
void writeFields(const MeshSource& v, const MeshSource* omit, FieldSink& out);
void writeFields(const MaterialOverride& v, const MaterialOverride* omit, FieldSink& out);
void writeFields(const Camera& v, const Camera* omit, FieldSink& out);
void writeFields(const Environment& v, const Environment* omit, FieldSink& out);
void writeFields(const Spin& v, const Spin* omit, FieldSink& out);
void writeFields(const InputActions& v, const InputActions* omit, FieldSink& out);
void writeFields(const FlyController& v, const FlyController* omit, FieldSink& out);
void writeFields(const Level& v, const Level* omit, FieldSink& out);
void writeFields(const Collider& v, const Collider* omit, FieldSink& out);
void writeFields(const RigidBody& v, const RigidBody* omit, FieldSink& out);
void writeFields(const SpawnPoint& v, const SpawnPoint* omit, FieldSink& out);
void writeFields(const Socket& v, const Socket* omit, FieldSink& out);
void writeFields(const PlayerController& v, const PlayerController* omit, FieldSink& out);
void writeFields(const Health& v, const Health* omit, FieldSink& out);
void writeFields(const Weapon& v, const Weapon* omit, FieldSink& out);
void writeFields(const WeaponState& v, const WeaponState* omit, FieldSink& out);
void writeFields(const WeaponTrigger& v, const WeaponTrigger* omit, FieldSink& out);
void writeFields(const Muzzle& v, const Muzzle* omit, FieldSink& out);
void writeFields(const ViewModel& v, const ViewModel* omit, FieldSink& out);
void writeFields(const MoveIntent& v, const MoveIntent* omit, FieldSink& out);
void writeFields(const Enemy& v, const Enemy* omit, FieldSink& out);
void writeFields(const EnemyState& v, const EnemyState* omit, FieldSink& out);
void writeFields(const Round& v, const Round* omit, FieldSink& out);

} // namespace ohao::ecs
