#pragma once
#include "render/scene/handles.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ohao::ecs {

struct Name { std::string value; };

// Local transform. Write it with registry.patch<Transform>() or World::setTransform();
// a plain get<>() write is not tracked (see verifyTransforms).
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    [[nodiscard]] glm::mat4 matrix() const;
};

// world: this frame's object-to-world. prevWorld: last frame's (motion vectors).
struct WorldTransform {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
};

struct Parent { entt::entity entity{entt::null}; };
struct Children { std::vector<entt::entity> entities; };

// Change tracking: like Transform, MeshRenderer, Light, EmissiveProxyLight and
// GiMaterialLegacy are picked up by the render extract only when written through
// registry.patch<T>() or registry.replace<T>() (or emplaced/removed). A plain
// get<T>() write fires no signal and never reaches the renderer.

// One material per submesh, in submesh order. Fewer entries than submeshes is legal:
// the missing submeshes render with the default material.
struct MeshRenderer {
    render::MeshHandle mesh{};
    std::vector<render::MaterialHandle> materials;
    bool visible{true};
};

enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };

// Position and direction come from WorldTransform: direction is the entity's -Z.
struct Light {
    LightType type{LightType::Point};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1.0f, 0.0f, 0.0f};   // AreaRect, local space
    glm::vec3 areaEdge2{0.0f, 0.0f, 1.0f};
};

// A point light approximating a mesh's emissive texture (ported from the renderer's
// emissive auto-light). localCenter/radius are in the entity's model space.
struct EmissiveProxyLight {
    glm::vec3 localCenter{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float intensity{0.0f};
};

// Monotonic creation sequence, set by World::create. Unlike entity ids it is never
// recycled, so it orders lights (and new instances) deterministically.
struct CreationOrder { uint64_t seq{0}; };

// LEGACY: RT GI's per-instance material word (see render::InstanceGpu::giAlbedoPacked).
// rgb = albedo, a = packed roughness/metallic/vertex-count rule. Set by spawnModel.
struct GiMaterialLegacy { glm::vec4 packed{0.73f, 0.73f, 0.73f, 0.95f}; };

struct TransformDirty {};
struct RenderDirty {};
struct MovedLastFrame {};

} // namespace ohao::ecs
