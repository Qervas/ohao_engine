#include "animated_rt_manager.hpp"
#include "gpu_skinning.hpp"
#include "rt_acceleration_structure.hpp"
#include "rt_gi_technique.hpp"
#include "rt_visibility.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/component/transform_component.hpp"
#include "scene/component/material_component.hpp"
#include "animation/animation_component.hpp"

namespace ohao {

void AnimatedRTManager::setDependencies(VkDevice device, VkCommandPool commandPool,
                                         VkQueue graphicsQueue, GPUSkinning* skinning,
                                         RTAccelerationStructure* rtAccel, RTGITechnique* rtGI) {
    m_device = device;
    m_commandPool = commandPool;
    m_graphicsQueue = graphicsQueue;
    m_skinning = skinning;
    m_rtAccel = rtAccel;
    m_rtGI = rtGI;
}

void AnimatedRTManager::registerMeshes(std::vector<AnimatedMeshInfo> meshes,
                                        std::vector<ActorBlasInfo> blasList) {
    m_animatedMeshes = std::move(meshes);
    m_actorBlasList = std::move(blasList);
}

bool AnimatedRTManager::update(Scene* scene, VkBuffer rtIndexBuffer, uint32_t totalVertexCount) {
    if (!scene || !m_skinning || !m_rtAccel || m_animatedMeshes.empty())
        return false;

    // Check if any animated actors are actually playing
    bool hasPlaying = false;
    for (const auto& am : m_animatedMeshes) {
        auto actorIt = scene->getAllActors().find(am.actorId);
        if (actorIt == scene->getAllActors().end()) continue;
        auto animComp = actorIt->second->getComponent<AnimationComponent>();
        if (animComp && animComp->isPlaying()) {
            hasPlaying = true;
            break;
        }
    }
    if (!hasPlaying) return false;

    // Step 1: Destroy previous frame's animated BLASes
    destroyOldBLASes();

    // Step 2-5: Record all GPU work on a single command buffer
    VkCommandBuffer skinCmd;
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = m_commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cmdAlloc, &skinCmd);

    VkCommandBufferBeginInfo oneShot{};
    oneShot.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    oneShot.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(skinCmd, &oneShot);

    computeSkinning(skinCmd, scene);
    rebuildBLASes(skinCmd, rtIndexBuffer, totalVertexCount);
    rebuildTLAS(skinCmd, scene);

    // Submit all GPU work
    vkEndCommandBuffer(skinCmd);
    VkSubmitInfo skinSubmit{};
    skinSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    skinSubmit.commandBufferCount = 1;
    skinSubmit.pCommandBuffers = &skinCmd;
    vkQueueSubmit(m_graphicsQueue, 1, &skinSubmit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &skinCmd);

    // CPU-side material sync (after GPU work completes)
    updateGIMaterials(scene);

    return true;
}

void AnimatedRTManager::cleanup() {
    destroyOldBLASes();
    m_animatedMeshes.clear();
    m_actorBlasList.clear();
}

// --- Private implementation ---

void AnimatedRTManager::destroyOldBLASes() {
    if (!m_rtAccel) return;
    for (auto blas : m_oldAnimatedBLAS)
        m_rtAccel->destroyBLAS(blas);
    m_oldAnimatedBLAS.clear();
}

void AnimatedRTManager::computeSkinning(VkCommandBuffer cmd, Scene* scene) {
    for (const auto& am : m_animatedMeshes) {
        auto actorIt = scene->getAllActors().find(am.actorId);
        if (actorIt == scene->getAllActors().end()) continue;
        auto animComp = actorIt->second->getComponent<AnimationComponent>();
        if (!animComp || !animComp->isPlaying()) continue;
        const auto& bones = animComp->getJointMatrices();
        if (!bones.empty())
            m_skinning->skin(cmd, am.skinHandle, bones);
    }
}

void AnimatedRTManager::rebuildBLASes(VkCommandBuffer cmd, VkBuffer rtIndexBuffer,
                                       uint32_t totalVertexCount) {
    m_rtAccel->ensureScratchBuffer(64 * 1024 * 1024);
    VkBuffer globalPosBuf = m_skinning->getGlobalSkinnedPositionBuffer();
    m_animatedBlasMap.clear();

    for (const auto& abi : m_actorBlasList) {
        if (!abi.isAnimated) continue;

        BlasHandle newBlas = m_rtAccel->createBLASFromPositions(
            globalPosBuf, totalVertexCount,
            rtIndexBuffer, abi.indexCount,
            abi.indexOffset * sizeof(uint32_t),
            cmd);
        if (newBlas != INVALID_BLAS) {
            m_animatedBlasMap[abi.actorId] = newBlas;
            m_oldAnimatedBLAS.push_back(newBlas);
        }

        // Barrier between BLAS builds for scratch buffer reuse
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
}

void AnimatedRTManager::rebuildTLAS(VkCommandBuffer cmd, Scene* scene) {
    m_rtAccel->clearInstances();
    m_giAlbedos.clear();
    m_giFlags.clear();

    uint32_t triOffset = 0;
    for (const auto& abi : m_actorBlasList) {
        auto actorIt = scene->getAllActors().find(abi.actorId);
        if (actorIt == scene->getAllActors().end()) continue;

        auto animIt = m_animatedBlasMap.find(abi.actorId);
        BlasHandle blas;
        if (animIt != m_animatedBlasMap.end()) {
            blas = animIt->second;
        } else if (abi.isAnimated) {
            continue;  // animated actor but no fresh BLAS — skip
        } else {
            blas = abi.originalBlas;
        }

        uint32_t mask = (animIt != m_animatedBlasMap.end()) ? rt::MASK_ANIMATED : rt::MASK_STATIC_ONLY;
        m_rtAccel->addInstance(blas, actorIt->second->getTransform()->getWorldMatrix(),
                               triOffset, mask);
        triOffset += abi.indexCount / 3;

        // Collect material info for GI sync
        auto matComp = actorIt->second->getComponent<MaterialComponent>();
        glm::vec3 color = matComp ? matComp->getMaterial().baseColor : glm::vec3(0.73f);
        float isStatic = (animIt == m_animatedBlasMap.end()) ? 1.0f : 0.0f;
        m_giAlbedos.push_back(color);
        m_giFlags.push_back(isStatic);
    }

    m_rtAccel->forceTlasRebuild();
    m_rtAccel->buildTLAS(cmd);
}

void AnimatedRTManager::updateGIMaterials(Scene* scene) {
    if (m_rtGI && !m_giAlbedos.empty()) {
        m_rtGI->setMaterialAlbedos(m_giAlbedos, m_giFlags);
    }
}

} // namespace ohao
