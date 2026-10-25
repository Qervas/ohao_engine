#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/scene/render_scene.hpp"
#include <glm/gtc/constants.hpp>

namespace ohao {

// The frame's raster lights: the scene's light table (clustered lighting uploads it to the
// frame slot only when its version changed), or the fallback sun when the scene has none,
// aligned with the sky's sun (or blended to the moon at night).
void VulkanRenderer::setFrameLights() {
    if (!m_deferredRenderer) return;
    float ambient = 0.35f;
    std::span<const LightData> lights;
    uint64_t version = 0;
    if (m_renderScene && !m_renderScene->rasterLights().empty()) {
        lights = m_renderScene->rasterLights();
        version = m_renderScene->rasterLightsVersion();
    } else {
        LightData& sun = m_fallbackSun;
        sun = LightData{};
        sun.position = glm::vec4(0.0f, 5.0f, 5.0f, 0.0f);   // type 0 = directional
        sun.direction = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f)), 100.0f);
        // Intensity = PI compensates for PBR energy-conserving divide-by-PI in diffuse BRDF
        sun.color = glm::vec4(1.0f, 0.98f, 0.95f, glm::pi<float>());
        // Shadowed through RT shadow channel 0 (render_dispatch traces the sun into it).
        sun.params = glm::vec4(0.0f, 0.0f, m_shadowsEnabled ? 0.0f : -1.0f,
                               m_shadowsEnabled ? 0.0f : render::kShadowNever);
        sun.lightSpaceMatrix = glm::mat4(1.0f);

        const glm::vec3 skyLightDir = m_deferredRenderer->getLightDirection();   // points DOWN toward the scene
        const float nightFactor = m_deferredRenderer->getNightFactor();
        if (glm::length(skyLightDir) > 0.001f) {
            if (nightFactor > 0.01f) {
                // Blend the direction from the sun toward the moon, the colour from warm white
                // to cool blue, the intensity from full (PI) to ~8% moonlight.
                const glm::vec3 moonDir = -m_deferredRenderer->getMoonDirection();
                const glm::vec3 blendedDir = glm::normalize(glm::mix(glm::normalize(skyLightDir), moonDir, nightFactor));
                sun.direction = glm::vec4(blendedDir, 100.0f);
                const glm::vec3 blendedColor = glm::mix(glm::vec3(1.0f, 0.98f, 0.95f), glm::vec3(0.6f, 0.7f, 0.85f), nightFactor);
                const float blendedIntensity = glm::mix(glm::pi<float>(), glm::pi<float>() * 0.08f, nightFactor);
                sun.color = glm::vec4(blendedColor, blendedIntensity);
            } else {
                sun.direction = glm::vec4(glm::normalize(skyLightDir), 100.0f);
            }
        }
        // Ambient: moderate during day, lower at night
        ambient = glm::mix(0.12f, 0.04f, nightFactor);
        lights = std::span<const LightData>(&sun, 1);
    }
    if (m_ambientOverride) ambient = *m_ambientOverride;
    m_deferredRenderer->setLights(lights, version, ambient);
}

} // namespace ohao
