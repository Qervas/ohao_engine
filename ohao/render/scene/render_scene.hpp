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
        RTAccelerationStructure* rt;   // null when RT is unavailable
    };
    RenderScene();
    ~RenderScene();
    RenderScene(const RenderScene&) = delete;
    RenderScene& operator=(const RenderScene&) = delete;

    [[nodiscard]] bool initialize(const InitInfo& info);
    void shutdown();

    MeshHandle registerMesh(const MeshData& data);            // uploads + BLAS now (blocking, load time)
    MaterialHandle registerMaterial(const MaterialDesc& desc);
    uint32_t createInstance(const InstanceDesc& desc);        // returns slot
    void updateInstance(uint32_t slot, const InstanceDesc& desc);
    void destroyInstance(uint32_t slot);
    void setLights(std::span<const LightDesc> lights);        // sorted by order inside
    // Bindless index of the environment map, stamped into the RT light header.
    void setEnvMapTextureIndex(uint32_t texIdx);

    // Start of a frame's command buffer, after the slot's fence was waited.
    void recordUpdates(VkCommandBuffer cmd, uint32_t frameSlot);
    // Called once per frame after the slot's fence wait (retire queue tick).
    void onFrameRetired();

    [[nodiscard]] const MeshInfo* meshInfo(MeshHandle h) const;
    [[nodiscard]] uint32_t materialIndexFor(uint32_t slot, uint32_t materialSlot) const;
    [[nodiscard]] bool rtSupported() const noexcept;

    // Buffers the passes bind.
    [[nodiscard]] VkBuffer vertexArena() const;
    [[nodiscard]] VkBuffer indexArena() const;
    [[nodiscard]] VkBuffer triangleSlotBuffer() const;
    [[nodiscard]] VkBuffer meshTable() const;
    [[nodiscard]] VkBuffer materialBuffer() const;
    [[nodiscard]] VkBuffer materialRefBuffer() const;
    [[nodiscard]] VkBuffer instanceBuffer() const;
    [[nodiscard]] std::span<const LightData> rasterLights() const;   // <= MAX_LIGHTS, host copy for the light UBO
    [[nodiscard]] VkBuffer rtLightBuffer() const;                    // header {count, envTexIdx} + GPULight[]
    [[nodiscard]] uint32_t rtLightCount() const;

    struct Draw { uint32_t slot; uint32_t firstIndex; uint32_t indexCount; int32_t vertexOffset; uint32_t materialSlot; glm::vec3 aabbMin, aabbMax; const glm::mat4* world; };
    // Visible instance x submesh draws; rebuilt only when instances change.
    [[nodiscard]] std::span<const Draw> draws() const;

    [[nodiscard]] const RenderSceneStats& stats() const noexcept;
    [[nodiscard]] VkAccelerationStructureKHR tlas() const;           // Task 5
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::render
