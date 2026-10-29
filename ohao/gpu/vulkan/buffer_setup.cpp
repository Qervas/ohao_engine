#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/scene/render_scene.hpp"
#include <glm/gtc/constants.hpp>

namespace ohao {

// The frame's raster lights: the scene's light table (clustered lighting uploads it to the
// frame slot only when its version changed). A scene without lights has none, in the deferred
// frame as in the path tracer: it is lit by its environment (or the constant ambient) alone.
void VulkanRenderer::setFrameLights() {
    if (!m_deferredRenderer) return;
    std::span<const LightData> lights;
    uint64_t version = 0;
    if (m_renderScene) {
        lights = m_renderScene->rasterLights();
        version = m_renderScene->rasterLightsVersion();
    }
    m_deferredRenderer->setLights(lights, version, m_ambientOverride.value_or(0.35f));
}

} // namespace ohao
