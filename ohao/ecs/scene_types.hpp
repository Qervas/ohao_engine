#pragma once
// The ECS side of reflection: registers every component with the type registry and
// teaches reflect that entt::entity fields are entity references. Include this header
// (not the registry alone) wherever ECS components are reflected, so the ValueTraits
// specialization below is visible to every instantiation.
//
// Never included by systems that run per frame (transform, render extract): reflection
// serves load, save, validation, commands and tools only.
#include "ecs/world.hpp"
#include "reflect/registry.hpp"

#include <entt/entt.hpp>

#include <cstdint>
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

// Registers every ECS component, authoring and runtime, in a fixed order (the order of
// the catalogue and of component blocks in scene files):
//   authoring (saved):  Name, Transform, Parent, Light
//   runtime (catalogue only): Children, WorldTransform, MeshRenderer, RenderSlot,
//     EmissiveProxyLight, GiMaterialLegacy, CreationOrder and the dirty tags.
// EntityId is not registered: it is the entity's key ("id" in scene files).
void registerSceneTypes(reflect::TypeRegistry& r);

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
