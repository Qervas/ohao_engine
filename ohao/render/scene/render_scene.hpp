#pragma once
#include "render/scene/render_scene_types.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <memory>

namespace ohao::render {

class MeshRegistry; class MaterialTable; class InstanceTable; class LightTable;

// The persistent GPU render world: mesh arenas (one BLAS per mesh), a material
// table, an instance table and a light table. Host edits mark entries dirty; the
// device-local tables are only ever written by copies recorded in recordUpdates,
// from a staging buffer owned by the frame slot, so a host write can never race a
// frame still in flight. Replaced buffers wait in a retire queue for
// MAX_FRAMES_IN_FLIGHT frames before they are destroyed.
class RenderScene {
public:
    struct InitInfo {
        VkInstance instance; VkPhysicalDevice physical; VkDevice device;
        VkQueue queue; uint32_t queueFamily; VkCommandPool commandPool;
        // Build BLASes on an RTAccelerationStructure owned by the RenderScene (never
        // shared with the legacy path, whose rebuild wipes and reuses BLAS indices).
        bool rayTracing;
        // VK_KHR_ray_query's rayQuery feature is enabled: compute and fragment shaders
        // may read the TLAS, so its post-build barrier must reach those stages too.
        bool rayQuery;
    };
    RenderScene();
    ~RenderScene();
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;

    [[nodiscard]] bool initialize(const InitInfo& info);
    void shutdown();

    MeshHandle registerMesh(const MeshData& data);            // uploads + BLAS now (blocking, load time)
    // The handle becomes stale (meshInfo returns null) and its BLAS is destroyed after
    // the frames in flight retire. Instances must stop using it first (the ECS extract
    // releases them when MeshRenderer changes). Arena space is not reused yet.
    void releaseMesh(MeshHandle h);
    MaterialHandle registerMaterial(const MaterialDesc& desc);
    uint32_t createInstance(const InstanceDesc& desc);        // returns slot
    void updateInstance(uint32_t slot, const InstanceDesc& desc);
    void destroyInstance(uint32_t slot);
    // Sorted by order inside. Each point, spot and area light also gets an emitter instance
    // (lightEmitter(): a sphere or its rectangle, kInstanceEmitter) that the camera sees as the
    // light's own radiance; it shadows nothing and lights nothing (the light does).
    void setLights(std::span<const LightDesc> lights);
    // Lights without their emitter meshes (true by default): the light still lights.
    void setLightEmitters(bool enabled);
    // Where the camera is this frame: a spot light's mesh shines toward it by its cone
    // (lightEmitter). Re-derives the spot emitters when it moves; nothing else depends on it.
    void setViewerPosition(const glm::vec3& viewer);
    // The emitter instances' slots, in light order (one per light that has one).
    [[nodiscard]] std::span<const uint32_t> lightEmitterSlots() const;
    // A live slot's instance as last written (host copy; uploaded by recordUpdates).
    [[nodiscard]] const InstanceGpu& instance(uint32_t slot) const;
    // Bindless index of the environment map, stamped into the RT light header.
    void setEnvMapTextureIndex(uint32_t texIdx);
    // HDRI scale stamped into the RT light header (pt_miss); default 1.
    void setEnvIntensity(float scale);

    // Start of a frame's command buffer, after the slot's fence was waited. Records
    // the table copies, then the TLAS build or refit (stats().lastTlasOp).
    void recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot);
    // Called once per frame after the slot's fence wait (retire queue tick).
    void onFrameRetired();

    [[nodiscard]] const MeshInfo* meshInfo(MeshHandle h) const;
    [[nodiscard]] uint32_t materialIndexFor(uint32_t slot, uint32_t materialSlot) const;
    [[nodiscard]] bool rtSupported() const noexcept;
    // The RenderScene's own acceleration structures (BLAS per mesh, TLAS over the
    // instance table). Null when RT is unavailable.
    [[nodiscard]] RTAccelerationStructure* rtAccel() const noexcept;

    // Buffers the passes bind.
    [[nodiscard]] VkBuffer vertexArena() const;
    [[nodiscard]] VkBuffer indexArena() const;
    [[nodiscard]] VkBuffer triangleSlotBuffer() const;
    [[nodiscard]] VkBuffer meshTable() const;
    [[nodiscard]] VkBuffer materialBuffer() const;
    [[nodiscard]] VkBuffer materialRefBuffer() const;
    [[nodiscard]] VkBuffer instanceBuffer() const;
    // The light list last given to setLights, sorted by order (host copy).
    [[nodiscard]] std::span<const LightDesc> lights() const;
    [[nodiscard]] std::span<const LightData> rasterLights() const;   // <= MAX_LIGHTS, host copy for the light table
    // Changes whenever rasterLights() does (clustered lighting uploads only then).
    [[nodiscard]] uint64_t rasterLightsVersion() const;
    // Shadow-casting raster lights past the kMaxShadowLights RT channels (lit unshadowed).
    [[nodiscard]] uint32_t shadowCastersWithoutChannel() const;
    // The lights RT shadows trace (<= kMaxShadowLights, element i = shadow-mask channel i).
    [[nodiscard]] std::span<const LightDesc> shadowLights() const;
    [[nodiscard]] VkBuffer rtLightBuffer() const;                    // header {count, envTexIdx} + GPULight[]
    [[nodiscard]] uint32_t rtLightCount() const;

    struct Draw { uint32_t slot; uint32_t firstIndex; uint32_t indexCount; int32_t vertexOffset; uint32_t materialSlot; glm::vec3 aabbMin, aabbMax; const glm::mat4* world; bool oneSided; uint32_t firstMeshlet; uint32_t meshletCount; bool emitter; bool viewModel; };
    // Visible instance x submesh draws, in slot order; rebuilt only when instances change.
    [[nodiscard]] std::span<const Draw> draws() const;
    // The same list on the GPU (gpu::shared::DrawRecord each), uploaded by recordUpdates:
    // what GPU culling reads and the vertex shaders index with firstInstance. Its
    // version changes whenever the list does (draw i is then another draw).
    [[nodiscard]] VkBuffer drawTable() const;
    [[nodiscard]] uint32_t drawCount() const;
    [[nodiscard]] uint64_t drawsVersion() const;
    // The draws' meshlets in draw order, each a "meshlet entry" (DrawRecord::aabbMin.w holds a
    // draw's first entry): what the mesh-shader fallback culls one by one. 0 while the table
    // is stale, as drawCount.
    [[nodiscard]] uint32_t meshletEntryCount() const;
    // The meshlet arenas (MeshRegistry).
    [[nodiscard]] VkBuffer meshletArena() const;
    [[nodiscard]] VkBuffer meshletVertexArena() const;
    [[nodiscard]] VkBuffer meshletTriangleArena() const;

    [[nodiscard]] const RenderSceneStats& stats() const noexcept;
    // TLAS over every slot below the instance high-water mark (custom index = slot;
    // released and invisible slots have mask 0). Built or refit by recordUpdates;
    // VK_NULL_HANDLE before the first build or without RT. The handle changes when
    // the TLAS outgrows its storage.
    [[nodiscard]] VkAccelerationStructureKHR tlas() const;
    // Host copy of the slot's TLAS instance transform (3x4 row-major), expanded.
    [[nodiscard]] glm::mat4 tlasTransform(uint32_t slot) const;
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::render
