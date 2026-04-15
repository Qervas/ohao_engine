#pragma once
// AnimatedRTManager — single class handling per-frame animated RT pipeline:
//   1. Update animation poses
//   2. GPU compute skinning
//   3. Fresh BLAS creation from skinned positions
//   4. TLAS rebuild with correct instance order
//   5. GI material buffer sync
//
// render_dispatch.cpp calls update() once per frame — one line instead of ~100.

#include "rt_acceleration_structure.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ohao {

class GPUSkinning;
class RTGITechnique;
class Scene;

// Metadata for one animated mesh registered with GPUSkinning
struct AnimatedMeshInfo {
    uint32_t skinHandle;     // GPUSkinning mesh handle
    uint64_t actorId;        // scene actor ID
};

// Per-actor BLAS metadata (built once during scene setup, used every frame)
struct ActorBlasInfo {
    uint64_t actorId;
    BlasHandle originalBlas;
    bool isAnimated;
    uint32_t indexCount;
    uint32_t indexOffset;
};

class AnimatedRTManager {
public:
    AnimatedRTManager() = default;
    ~AnimatedRTManager() = default;

    // Set external dependencies (does not own these)
    void setDependencies(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue,
                         GPUSkinning* skinning, RTAccelerationStructure* rtAccel,
                         RTGITechnique* rtGI);

    // Register animated meshes and build actor BLAS list (called once during scene setup)
    void registerMeshes(std::vector<AnimatedMeshInfo> meshes,
                        std::vector<ActorBlasInfo> blasList);

    // Per-frame update: skinning → BLAS → TLAS → materials
    // Returns true if dynamic BLAS rebuild was performed
    bool update(Scene* scene, VkBuffer rtIndexBuffer, uint32_t totalVertexCount);

    // Access for read
    const std::vector<AnimatedMeshInfo>& getAnimatedMeshes() const { return m_animatedMeshes; }
    const std::vector<ActorBlasInfo>& getActorBlasList() const { return m_actorBlasList; }
    bool hasAnimatedContent() const { return !m_animatedMeshes.empty(); }

    // Cleanup old BLAS handles (call during shutdown)
    void cleanup();

private:
    void destroyOldBLASes();
    void computeSkinning(VkCommandBuffer cmd, Scene* scene);
    void rebuildBLASes(VkCommandBuffer cmd, VkBuffer rtIndexBuffer, uint32_t totalVertexCount);
    void rebuildTLAS(VkCommandBuffer cmd, Scene* scene);
    void updateGIMaterials(Scene* scene);

    // External dependencies (not owned)
    VkDevice m_device{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    GPUSkinning* m_skinning{nullptr};
    RTAccelerationStructure* m_rtAccel{nullptr};
    RTGITechnique* m_rtGI{nullptr};

    // Registered data
    std::vector<AnimatedMeshInfo> m_animatedMeshes;
    std::vector<ActorBlasInfo> m_actorBlasList;

    // Per-frame state
    std::vector<BlasHandle> m_oldAnimatedBLAS;
    std::unordered_map<uint64_t, BlasHandle> m_animatedBlasMap;

    // GI material sync
    std::vector<glm::vec3> m_giAlbedos;
    std::vector<float> m_giFlags;
};

} // namespace ohao
