#pragma once
// The ECS side of reflection: registers every component with the type registry and
// teaches reflect that entt::entity fields are entity references. Include this header
// (not the registry alone) wherever ECS components are reflected, so the ValueTraits
// specialization below is visible to every instantiation.
//
// Never included by systems that run per frame (transform, render extract): reflection
// serves load, save, validation, commands and tools only.
#include "ecs/snapshot_column.hpp"
#include "ecs/world.hpp"
#include "reflect/registry.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <optional>

template <>
struct ohao::reflect::ValueTraits<entt::entity> {
    static constexpr bool isEntityRef = true;
    static uint64_t toRaw(entt::entity e) noexcept { return static_cast<uint64_t>(entt::to_integral(e)); }
    static entt::entity fromRaw(uint64_t raw) noexcept {
        return static_cast<entt::entity>(static_cast<entt::id_type>(raw));
    }
    static constexpr entt::entity null() noexcept { return entt::null; }
};

namespace ohao::ecs {

// What scene load/save, commands and queries need to move a component between a typed
// value and the registry, generated at registration and stored in TypeInfo::host. Every
// component with data carries it (saved and runtime); empty tag types do not.
struct ComponentOps {
    void (*emplace)(entt::registry& r, entt::entity e, void* value);   // moves from value; fires signals
    const void* (*tryGet)(const entt::registry& r, entt::entity e);
    void (*remove)(entt::registry& r, entt::entity e);
    std::size_t (*count)(const entt::registry& r);
    // Snapshots (ecs/snapshot.hpp): capture every T into a column (rowOf maps an entity
    // index to its snapshot row), and restore a column by diffing (see restoreColumn).
    void (*capture)(const entt::registry& r, std::span<const uint32_t> rowOf, SnapshotColumn& out);
    std::size_t (*restore)(entt::registry& r, const SnapshotColumn& col, RestoreScratch& scratch);
    // tryGet without the registry's per-call storage lookup: `pool` once per pass (null
    // when the registry has no storage for T), then `tryGetIn` per entity.
    const void* (*pool)(const entt::registry& r);
    const void* (*tryGetIn)(const void* pool, entt::entity e);
    // world.hash: every T in snapshot row order (see hashColumn). world.diff: the values
    // that differ between a snapshot column and the live world (see diffColumn).
    void (*hash)(const entt::registry& r, std::span<const uint32_t> rowOf, const reflect::TypeInfo& t,
                 reflect::Hasher& h);
    void (*diff)(const entt::registry& r, const SnapshotColumn& col, DiffScratch& scratch,
                 std::vector<ColumnChange>& out);
};


template <class T>
[[nodiscard]] std::shared_ptr<const void> makeComponentOps() {
    static const ComponentOps ops{
        [](entt::registry& r, entt::entity e, void* v) { r.emplace_or_replace<T>(e, std::move(*static_cast<T*>(v))); },
        [](const entt::registry& r, entt::entity e) -> const void* { return r.try_get<T>(e); },
        [](entt::registry& r, entt::entity e) { r.remove<T>(e); },
        [](const entt::registry& r) -> std::size_t {
            const auto* s = r.storage<T>();
            return s ? s->size() : 0;
        },
        &detail::captureColumn<T>,
        &detail::restoreColumn<T>,
        [](const entt::registry& r) -> const void* { return r.storage<T>(); },
        [](const void* pool, entt::entity e) -> const void* {
            using Pool = std::remove_cvref_t<decltype(*std::declval<const entt::registry&>().template storage<T>())>;
            const auto* s = static_cast<const Pool*>(pool);
            return s && s->contains(e) ? &s->get(e) : nullptr;
        },
        &detail::hashColumn<T>,
        &detail::diffColumn<T>,
    };
    return std::shared_ptr<const void>(&ops, [](const void*) {});
}

[[nodiscard]] inline const ComponentOps* componentOps(const reflect::TypeInfo& t) noexcept {
    return static_cast<const ComponentOps*>(t.host.get());
}

// Registers every ECS component, authoring and runtime, in a fixed order (the order of
// the catalogue and of component blocks in scene files):
//   authoring (saved):  Name, Transform, Parent, Light, MeshSource, Material,
//     Camera, Environment (both singletons), Spin, InputActions (singleton),
//     FlyController, Level, SpawnPoint, Socket. MeshSource's alternatives ModelAsset and
//     the *Prim structs, and ActionBinding/AxisBinding, are nested types.
//   runtime (catalogue only): Children, WorldTransform, MeshRenderer, RenderSlot,
//     CreationOrder, PreservedComponents,
//     InstantiateError, LevelNode and the dirty tags.
// EntityId is not registered: it is the entity's key ("id" in scene files).
void registerSceneTypes(reflect::TypeRegistry& r);
// The Key enum (core/input.hpp's table), for any registry whose types name keys.
void registerKeyEnum(reflect::TypeRegistry& r);
// The engine's event payloads (ecs/event_types.cpp; registerSceneTypes calls it).
void registerEventTypes(reflect::TypeRegistry& r);

// Writes entity references as the target's EntityId; dead or id-less targets as null.
class WorldEntityIds final : public reflect::EntityIdMapper {
public:
    explicit WorldEntityIds(const World& w) : m_world(w) {}
    [[nodiscard]] std::optional<uint64_t> idOf(uint64_t rawEntity) const override {
        const uint64_t id = m_world.idOf(reflect::ValueTraits<entt::entity>::fromRaw(rawEntity));
        return id ? std::optional<uint64_t>(id) : std::nullopt;
    }

private:
    const World& m_world;
};

} // namespace ohao::ecs
