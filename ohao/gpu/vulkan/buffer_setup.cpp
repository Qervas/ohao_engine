#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include "scene/component/light_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "scene/asset/model.hpp"
#include "render/scene/light_table.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace ohao {

bool VulkanRenderer::createLightBuffer() {
    VkDeviceSize bufferSize = sizeof(LightUniformBuffer);

    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufferSize,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_lightBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_lightBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_lightBufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_lightBuffer, m_lightBufferMemory, 0);

    // Keep buffer mapped for easy updates
    vkMapMemory(m_device, m_lightBufferMemory, 0, bufferSize, 0, &m_lightBufferMapped);

    // Initialize with default light
    updateLightBuffer();

    return true;
}

glm::mat4 VulkanRenderer::calculateLightSpaceMatrix(const LightData& light) {
    return render::lightSpaceMatrix(light);
}

void VulkanRenderer::updateLightBuffer() {
    LightUniformBuffer lightUbo{};
    lightUbo.numLights = 0;
    lightUbo.ambientIntensity = 0.35f;
    lightUbo.shadowBias = 0.005f;
    lightUbo.shadowStrength = 0.7f;

    int shadowCasterIndex = -1;  // Index of first shadow-casting light

    // Collect lights from scene
    if (m_scene) {
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto lightComp = actor->getComponent<LightComponent>();
            if (!lightComp) continue;

            if (lightUbo.numLights >= static_cast<int>(MAX_LIGHTS)) break;

            LightData& light = lightUbo.lights[lightUbo.numLights];

            glm::vec3 worldPos = actor->getTransform()->getPosition();
            glm::vec3 worldDir = lightComp->getDirection();

            // Map: Sphere=0→Point=1, Directional=1→Dir=0, Spot=2→Spot=2
            int deferredType = (lightComp->getLightType() == LightType::Sphere) ? 1 : (lightComp->getLightType() == LightType::Directional) ? 0 : 2;
            light.position = glm::vec4(worldPos, static_cast<float>(deferredType));
            light.direction = glm::vec4(worldDir, lightComp->getRange());
            light.color = glm::vec4(lightComp->getColor(), lightComp->getIntensity());

            float shadowMapIndex = -1.0f;
            if (m_shadowsEnabled && shadowCasterIndex < 0) {
                int lightType = static_cast<int>(lightComp->getLightType());
                if (lightType == 0 || lightType == 2) {
                    shadowMapIndex = 0.0f;
                    shadowCasterIndex = lightUbo.numLights;
                }
            }

            light.params = glm::vec4(
                glm::cos(glm::radians(lightComp->getInnerConeAngle())),
                glm::cos(glm::radians(lightComp->getOuterConeAngle())),
                shadowMapIndex,
                0.0f
            );

            if (shadowMapIndex >= 0.0f) {
                light.lightSpaceMatrix = calculateLightSpaceMatrix(light);
            } else {
                light.lightSpaceMatrix = glm::mat4(1.0f);
            }

            lightUbo.numLights++;
        }
    }

    // Append cached emissive mesh lights (computed once in updateSceneBuffers)
    for (const auto& emLight : m_cachedEmissiveLights) {
        if (lightUbo.numLights >= static_cast<int>(MAX_LIGHTS)) break;
        lightUbo.lights[lightUbo.numLights] = emLight;
        lightUbo.numLights++;
    }

    // If no lights in scene, add a default directional light (direction synced by renderDeferred)
    if (lightUbo.numLights == 0) {
        LightData& defaultLight = lightUbo.lights[0];
        defaultLight.position = glm::vec4(0.0f, 5.0f, 5.0f, 0.0f);  // type 0 = directional
        defaultLight.direction = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f)), 100.0f);
        // Intensity = PI compensates for PBR energy-conserving divide-by-PI in diffuse BRDF
        defaultLight.color = glm::vec4(1.0f, 0.98f, 0.95f, glm::pi<float>());
        defaultLight.params = glm::vec4(0.0f, 0.0f, m_shadowsEnabled ? 0.0f : -1.0f, 0.0f);

        if (m_shadowsEnabled) {
            defaultLight.lightSpaceMatrix = calculateLightSpaceMatrix(defaultLight);
        } else {
            defaultLight.lightSpaceMatrix = glm::mat4(1.0f);
        }

        lightUbo.numLights = 1;
    }

    memcpy(m_lightBufferMapped, &lightUbo, sizeof(lightUbo));
}

void VulkanRenderer::cacheEmissiveLights() {
    m_cachedEmissiveLights.clear();
    if (!m_scene) return;

    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto mc = actor->getComponent<MeshComponent>();
        if (!mc || !mc->getModel()) continue;
        auto model = mc->getModel();

        for (size_t mi = 0; mi < model->materialEmissiveTexIndex.size(); mi++) {
            if (model->materialEmissiveTexIndex[mi] < 0) continue;

            // Compute world-space mesh center and bounding radius
            glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
            for (const auto& v : model->vertices) {
                bmin = glm::min(bmin, v.position);
                bmax = glm::max(bmax, v.position);
            }
            glm::mat4 worldMat = actor->getTransform()->getWorldMatrix();
            glm::vec3 center = glm::vec3(worldMat * glm::vec4((bmin + bmax) * 0.5f, 1.0f));
            float radius = glm::length(bmax - bmin) * 0.3f;
            float range = radius * 10.0f;

            // Compute average emissive color from bright pixels
            glm::vec3 emColor(1.0f);
            float totalPower = 0.0f;
            int eTexIdx = model->materialEmissiveTexIndex[mi];
            if (eTexIdx >= 0 && eTexIdx < static_cast<int>(model->emissiveTextures.size())) {
                const auto& etd = model->emissiveTextures[eTexIdx];
                double r = 0, g = 0, b = 0;
                int brightPixels = 0;
                for (int p = 0; p < etd.width * etd.height; p++) {
                    float pr = etd.pixels[p*4+0] / 255.0f;
                    float pg = etd.pixels[p*4+1] / 255.0f;
                    float pb = etd.pixels[p*4+2] / 255.0f;
                    float lum = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;
                    if (lum > 0.05f) {
                        r += pr; g += pg; b += pb;
                        brightPixels++;
                        totalPower += lum;
                    }
                }
                if (brightPixels > 0) {
                    emColor = glm::vec3(r / brightPixels, g / brightPixels, b / brightPixels);
                }
            }

            if (totalPower > 0.1f) {
                float intensity = std::min(totalPower * 0.1f, 20.0f);

                LightData light{};
                light.position = glm::vec4(center, 1.0f);  // type 1 = point light
                light.direction = glm::vec4(0, -1, 0, range);
                light.color = glm::vec4(emColor, intensity);
                light.params = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
                light.lightSpaceMatrix = glm::mat4(1.0f);
                m_cachedEmissiveLights.push_back(light);

                std::cout << "[Deferred] Emissive mesh light: " << actor->getName()
                          << " color=(" << emColor.r << "," << emColor.g << "," << emColor.b
                          << ") intensity=" << intensity << std::endl;
            }
            break;  // one light per actor
        }
    }
}

} // namespace ohao
