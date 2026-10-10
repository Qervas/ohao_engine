#include "renderer_impl.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/scene.hpp"

#include <iostream>

namespace ohao {

bool VulkanRenderer::updateSceneBuffers() {
    if (!m_scene) {
        std::cout << "Cannot update buffers - no scene exists" << std::endl;
        m_hasSceneMeshes = false;
        return false;
    }

    // Frames in flight may still read the bridge's instances and textures.
    vkDeviceWaitIdle(m_device);

    // Everything the deferred and RT passes read lives in m_renderScene, fed by the
    // TEMPORARY bridge from the old Scene; the env map joins the bindless table.
    bridgeSceneToRenderScene();
    uploadEnvironmentMap();

    m_hasSceneMeshes = !m_bridgeInstances.empty();
    if (!m_hasSceneMeshes) {
        std::cout << "No actors with mesh components found in scene" << std::endl;
        return false;
    }
    std::cout << "Scene uploaded: " << m_bridgeInstances.size() << " instances" << std::endl;
    return true;
}

bool VulkanRenderer::updateRTLightParams() {
    if (!m_scene || !m_renderScene) return false;
    syncBridgedScene();   // rebuilds the light list; the env index and scale stay
    m_renderScene->setEnvIntensity(m_envIntensityScale);
    return true;
}

} // namespace ohao
