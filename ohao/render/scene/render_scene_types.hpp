#pragma once
#include "render/scene/handles.hpp"
#include "render/rt/gpu_light.hpp"
#include "render/rt/rt_acceleration_structure.hpp"
#include "scene/asset/model.hpp"   // Vertex
#include "gpu/layout_meta.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace ohao {

// Light data for the deferred light uniform buffer (matches UnifiedLight in shaders).
// 128 bytes per light.
struct LightData {
    alignas(16) glm::vec4 position;      // xyz = position, w = type (0=dir, 1=point, 2=spot)
    alignas(16) glm::vec4 direction;     // xyz = direction, w = range
    alignas(16) glm::vec4 color;         // xyz = color, w = intensity
    alignas(16) glm::vec4 params;        // x = innerCone, y = outerCone, z = shadowMapIndex (-1=none), w = shadow slot (kShadowNever, 0..7)
    alignas(16) glm::mat4 lightSpaceMatrix; // Transform to light space for shadow mapping (64 bytes)
};
OHAO_ASSERT_GPU_LAYOUT(LightData, layout::kLightDataBytes);

// Maximum lights in the deferred light uniform buffer.
constexpr uint32_t MAX_LIGHTS = 8;

} // namespace ohao

namespace ohao::render {

inline constexpr uint32_t kNoTexture = 0xFFFFFFFFu;
inline constexpr uint32_t kMaxMaterialRefsPerInstance = 64;
static_assert(sizeof(Vertex) == 100, "RT shaders read Vertex as 25 floats");

struct SubmeshDesc { uint32_t firstIndex; uint32_t indexCount; uint32_t materialSlot; };

struct MeshData {
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;           // mesh-local
    std::vector<SubmeshDesc> submeshes;          // empty: one submesh, slot 0
    std::vector<uint32_t> triangleMaterialSlot;  // empty: all 0
    std::string debugName;
};

struct MaterialDesc {
    glm::vec3 baseColor{0.8f};
    float roughness{0.5f};
    float metallic{0.0f};
    uint32_t albedoTex{kNoTexture};
    uint32_t normalTex{kNoTexture};
    uint32_t roughMetalTex{kNoTexture};
    uint32_t emissiveTex{kNoTexture};
    float emissiveStrength{0.0f};
    float subsurface{0.0f};   // > 0: the SSS blur's strength on this material's pixels (table row 2, z)
};

struct InstanceDesc {
    MeshHandle mesh;
    std::span<const MaterialHandle> materials;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    bool visible{true};
    glm::vec4 giAlbedoPacked{0.73f, 0.73f, 0.73f, 0.95f};   // see InstanceGpu
};

enum class LightKind : uint32_t { Point, Directional, Spot, AreaRect, EmissiveProxy };

// World-space light, already resolved from the ECS. `order` sorts the light list
// (creation sequence) so both light buffers are deterministic.
struct LightDesc {
    LightKind kind{LightKind::Point};
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1, 0, 0};
    glm::vec3 areaEdge2{0, 0, 1};
    bool castShadows{true};   // emissive proxies never do
    uint64_t order{0};
};

// RT shadows know at most this many lights, one visibility channel each (LightData::params.w
// names a raster light's channel): every shadow-casting light of the deferred light buffer.
// A pixel traces at most gpu::shared::kShadowLightsPerPixel of them, the most important.
inline constexpr uint32_t kMaxShadowLights = MAX_LIGHTS;
// LightData::params.w: -1 never shadowed (castShadows false, emissive proxies);
// 0..kMaxShadowLights-1 its RT visibility channel (CSM shadows a directional one without RT).
inline constexpr float kShadowNever = -1.0f;

// A point light approximating a mesh's emissive texture (ported from the renderer's
// emissive auto-light). localCenter/radius are in the mesh's model space; the light
// table turns radius into range = radius * 10.
struct EmissiveProxy {
    glm::vec3 localCenter{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float intensity{0.0f};
};

// std430, matches shaders/includes/scene/scene_data.glsl
struct InstanceGpu {
    glm::mat4 world;
    glm::mat4 prevWorld;
    uint32_t mesh;
    uint32_t materialRefOffset;
    uint32_t materialRefCount;
    uint32_t flags;          // bit 0 visible
    // RT GI's bounce albedo: rgb, read by rt/gi_trace.slang at a hit (kInstanceGpuGiAlbedoOffset
    // in shaders/shared/layout.json). a = the legacy packed word: metallic>0.5 ?
    // -(roughness+0.001) : roughness, then +/-10 when the mesh has > 100 vertices; nothing
    // reads it.
    glm::vec4 giAlbedoPacked;
};
static_assert(sizeof(InstanceGpu) == 160);

inline constexpr uint32_t kInstanceVisible = 1u;

struct MeshGpu {
    uint32_t vertexOffset;        // in vertices
    uint32_t indexOffset;         // in indices
    uint32_t indexCount;
    uint32_t triangleSlotOffset;  // into the triangle material-slot stream
};
static_assert(sizeof(MeshGpu) == 16);

struct MeshInfo {
    uint32_t vertexOffset, vertexCount, indexOffset, indexCount, triangleSlotOffset;
    std::vector<SubmeshDesc> submeshes;
    glm::vec3 aabbMin, aabbMax;
    BlasHandle blas{INVALID_BLAS};
    std::string debugName;
    bool released{false};   // RenderScene::releaseMesh
};

struct RenderSceneStats {
    std::size_t instanceBytesUploaded{0};   // last recordUpdates
    std::size_t lightBytesUploaded{0};
    double recordCpuMs{0.0};                // CPU time of the last recordUpdates (staging writes + copy recording)
    uint32_t arenaGrowths{0};               // cumulative, mesh arenas (load time)
    uint32_t instanceTableGrowths{0};       // cumulative, device instance/material-ref buffers (mid-frame)
    enum class TlasOp { None, Build, Refit } lastTlasOp{TlasOp::None};
};

namespace detail {

// One frame slot's host-visible staging buffer while recordUpdates fills it. The
// tables append their dirty bytes and record one copy per contiguous run into
// their device-local buffers. Every GPU struct the tables upload is a multiple of
// 16 bytes, so runs stay 16-byte aligned without padding.
struct StagingBatch {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    VkBuffer buffer{VK_NULL_HANDLE};
    std::byte* mapped{nullptr};
    VkDeviceSize used{0};
    VkDeviceSize capacity{0};

    // Copies `bytes` from `src` into staging, records the copy to `dst` at `dstOffset`.
    void upload(VkBuffer dst, VkDeviceSize dstOffset, const void* src, VkDeviceSize bytes) {
        if (bytes == 0) return;
        assert(used + bytes <= capacity && "staging sized from pendingBytes()");
        std::memcpy(mapped + used, src, static_cast<std::size_t>(bytes));
        const VkBufferCopy region{used, dstOffset, bytes};
        vkCmdCopyBuffer(cmd, buffer, dst, 1, &region);
        used += bytes;
    }
};

} // namespace detail

} // namespace ohao::render
