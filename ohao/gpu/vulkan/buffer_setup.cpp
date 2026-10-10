#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include "render/scene/light_table.hpp"
#include "render/scene/render_scene.hpp"
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

    // Scene lights, resolved by the RenderScene's light table (regular lights in
    // order, the first point/spot light as shadow caster, then emissive proxies).
    if (m_renderScene) {
        for (const LightData& light : m_renderScene->rasterLights()) {
            if (lightUbo.numLights >= static_cast<int>(MAX_LIGHTS)) break;
            lightUbo.lights[lightUbo.numLights++] = light;
        }
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

} // namespace ohao
