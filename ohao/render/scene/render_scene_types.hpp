#pragma once
#include "render/scene/handles.hpp"
#include "render/rt/gpu_light.hpp"
#include "render/rt/rt_acceleration_structure.hpp"
#include "scene/asset/model.hpp"   // Vertex
#include "gpu/layout_meta.hpp"
#include "gpu/shared_layout.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
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
    alignas(16) glm::vec4 params;        // x = cos innerCone, y = cos outerCone, z = sphere radius (0: a point), w = shadow slot (kShadowNever, 0..7)
    alignas(16) glm::mat4 lightSpaceMatrix; // Transform to light space for shadow mapping (64 bytes)
};
OHAO_ASSERT_GPU_LAYOUT(LightData, layout::kLightDataBytes);

// The raster light table's capacity (clustered lighting: gpu::shared::kLightCapacity).
constexpr uint32_t MAX_LIGHTS = 65536;

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

// glTF's alphaMode (gpu::shared::kAlphaOpaque / kAlphaMask / kAlphaBlend). Mask: alpha-tested
// against alphaCutoff in the G-buffer (a discard pipeline) and by every ray; Blend: drawn by the
// forward pass (transparent), never in the G-buffer, Hi-Z or CSM, seen by camera rays only.
enum class AlphaMode : uint8_t { Opaque = 0, Mask = 1, Blend = 2 };

// A material: OpenPBR Surface 1.1.1's base (shaders/material/openpbr.slang), its defaults.
// roughness is specular_roughness, metallic base_metalness, emissiveStrength
// emission_luminance.
struct MaterialDesc {
    glm::vec3 baseColor{0.8f};
    float roughness{0.3f};
    float metallic{0.0f};
    float baseWeight{1.0f};
    float diffuseRoughness{0.0f};
    float specularWeight{1.0f};
    glm::vec3 specularColor{1.0f};
    float specularIor{1.5f};
    glm::vec3 emissionColor{1.0f};
    uint32_t albedoTex{kNoTexture};
    uint32_t normalTex{kNoTexture};
    uint32_t roughMetalTex{kNoTexture};
    uint32_t emissiveTex{kNoTexture};
    // glTF's occlusionTexture: ambient occlusion in its R channel (table row 2, w). Often the
    // same image as roughMetalTex (packed ORM), never implied by it: a metallicRoughness map's
    // R channel is unused by glTF.
    uint32_t occlusionTex{kNoTexture};
    // KHR_materials_specular's textures: the weight in A, the colour in RGB (sRGB).
    uint32_t specularTex{kNoTexture};
    uint32_t specularColorTex{kNoTexture};
    float emissiveStrength{0.0f};
    float subsurface{0.0f};   // > 0: the SSS blur's strength on this material's pixels (table row 2, z)
    // False: one-sided, its back faces culled in raster (glTF's doubleSided: false). Materials
    // that never say (scene-file materials, OBJ, FBX) stay double-sided.
    bool doubleSided{true};
    // Alpha (table row 3): the mode, the mask's cutoff, an opacity factor (Material's geometry_opacity) and
    // the base colour's alpha (glTF baseColorFactor.a). Coverage = baseAlpha x the albedo
    // texture's alpha x opacity.
    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{0.5f};
    float opacity{1.0f};
    float baseAlpha{1.0f};
};

struct InstanceDesc {
    MeshHandle mesh;
    std::span<const MaterialHandle> materials;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    bool visible{true};
    // A light's emitter mesh (RenderScene makes them from the lights; kInstanceEmitter):
    // emitterLight is then its radiance and light.
    bool emitter{false};
    // A first-person gun's mesh (ecs ViewModel; kInstanceViewModel): drawn and lit as any mesh, but
    // in no CSM cascade and no shadow, GI or bounce ray (its pose is shrunk toward the eye).
    bool viewModel{false};
    glm::vec4 emitterLight{0.0f};   // see InstanceGpu
};

// Flash: an effect's light (vfx_draw's flashes): a point in the raster table only, with no emitter mesh
// and no shadow, its range its radius x 10; neither the path tracer nor RT GI sees it.
enum class LightKind : uint32_t { Point, Directional, Spot, AreaRect, Flash };

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
    bool castShadows{true};   // flashes never do
    uint64_t order{0};
    // Where the light was last frame, as the transform taking this frame's world space to it
    // (the entity's prevWorld * inverse(world)): its emitter mesh's prevWorld is this times its
    // world, so the mesh writes motion vectors as any moved mesh does. Identity when still.
    glm::mat4 lastFrameMotion{1.0f};
};

// LightData::params.w: 1 a light that casts shadows, 0 one that does not, kShadowNever a flash (RT
// direct light lights every light but flashes, shadowed; without it CSM shadows a casting sun).
inline constexpr float kShadowNever = -1.0f;

// std430, matches shaders/includes/scene/scene_data.slang
struct InstanceGpu {
    glm::mat4 world;
    glm::mat4 prevWorld;
    uint32_t mesh;
    uint32_t materialRefOffset;
    uint32_t materialRefCount;
    uint32_t flags;          // kInstanceVisible, kInstanceEmitter, kInstanceViewModel
    // A light's emitter mesh (kInstanceEmitter): rgb its radiance (the G-buffer and both closest
    // hits), w its light's index in the RT light list (the reference path tracer weighs a bounce
    // ray that meets it against that light's sampling). Zero on every other instance.
    glm::vec4 emitterLight;
};
static_assert(sizeof(InstanceGpu) == 160);

inline constexpr uint32_t kInstanceVisible = 1u;
// A light's emitter mesh: pure emission, seen by camera rays and the G-buffer only.
inline constexpr uint32_t kInstanceEmitter = gpu::shared::kInstanceEmitter;
// A first-person gun's mesh: no CSM cascade, no shadow, GI or bounce ray.
inline constexpr uint32_t kInstanceViewModel = gpu::shared::kInstanceViewModel;

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
    // Per submesh: its first meshlet in the meshlet arena and its meshlet count.
    std::vector<std::array<uint32_t, 2>> submeshMeshlets;
    glm::vec3 aabbMin, aabbMax;
    BlasHandle blas{INVALID_BLAS};
    std::string debugName;
    bool released{false};   // RenderScene::releaseMesh
    // A skinned instance's own mesh (RenderScene::registerSkinnedInstance): its vertices are the
    // base mesh's deformed by the skin pass, its indices and triangle slots the base's, its
    // meshlets copies of the base's (spheres rewritten by skin.bounds), its BLAS updatable.
    bool skinned{false};
    uint32_t baseVertexOffset{0};   // the base mesh's first vertex (the bind pose the pass reads)
    uint32_t firstMeshlet{0}, meshletCount{0};   // its own meshlet copies, all submeshes
};

// A skinned mesh's BLAS is refit while its pose changes and rebuilt after this many refits (the
// spec's recommendation, OPEN ruling 12: about 2 s at 60 Hz).
inline constexpr uint32_t kBlasRefitsPerRebuild = 120;

struct RenderSceneStats {
    std::size_t instanceBytesUploaded{0};   // last recordUpdates
    std::size_t lightBytesUploaded{0};
    double recordCpuMs{0.0};                // CPU time of the last recordUpdates (staging writes + copy recording)
    uint32_t arenaGrowths{0};               // cumulative, mesh arenas (load time)
    uint32_t instanceTableGrowths{0};       // cumulative, device instance/material-ref buffers (mid-frame)
    enum class TlasOp { None, Build, Refit } lastTlasOp{TlasOp::None};
    // The skinned instances, last recordSkinning: those whose pose changed were skinned (vertices
    // their vertex count), their BLASes refit or rebuilt; the counts are cumulative.
    struct Skinning {
        uint32_t instances{0};     // skinned instances registered
        uint32_t skinned{0};       // skinned last recordSkinning (pose changed)
        uint64_t vertices{0};      // vertices skinned last recordSkinning
        uint64_t blasRefits{0};    // cumulative
        uint64_t blasRebuilds{0};  // cumulative
        uint64_t blasUpdateCalls{0};   // cumulative: build calls recording them (one a frame, every BLAS in it)
    } skinning;
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
