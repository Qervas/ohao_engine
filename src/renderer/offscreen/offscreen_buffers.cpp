#include "offscreen_renderer_impl.hpp"
#include "renderer/camera/camera.hpp"
#include "renderer/components/light_component.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/asset/model.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace ohao {

bool OffscreenRenderer::createUniformBuffer() {
    VkDeviceSize bufferSize = sizeof(CameraUniformBuffer);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_uniformBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_uniformBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_uniformBufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_uniformBuffer, m_uniformBufferMemory, 0);

    // Keep buffer mapped for easy updates
    vkMapMemory(m_device, m_uniformBufferMemory, 0, bufferSize, 0, &m_uniformBufferMapped);

    return true;
}

bool OffscreenRenderer::createVertexBuffer() {
    // Create a demo triangle using full Vertex struct
    std::vector<Vertex> vertices = {
        // Bottom vertex - Red
        {{0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}},
        // Top right - Green
        {{0.5f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        // Top left - Blue
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}}
    };

    m_vertexCount = static_cast<uint32_t>(vertices.size());
    VkDeviceSize bufferSize = sizeof(Vertex) * vertices.size();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);

    // Copy vertex data
    void* data;
    vkMapMemory(m_device, m_vertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), bufferSize);
    vkUnmapMemory(m_device, m_vertexBufferMemory);

    std::cout << "Created vertex buffer with " << m_vertexCount << " vertices" << std::endl;
    return true;
}

void OffscreenRenderer::updateUniformBuffer() {
    CameraUniformBuffer ubo{};
    ubo.view = m_camera->getViewMatrix();
    ubo.proj = glm::perspective(glm::radians(45.0f),
                                static_cast<float>(m_width) / static_cast<float>(m_height),
                                0.1f, 100.0f);
    // Flip Y for Vulkan
    ubo.proj[1][1] *= -1;
    ubo.viewPos = m_camera->getPosition();

    memcpy(m_uniformBufferMapped, &ubo, sizeof(ubo));
}

bool OffscreenRenderer::createLightBuffer() {
    VkDeviceSize bufferSize = sizeof(LightUniformBuffer);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_lightBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, m_lightBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

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

void OffscreenRenderer::updateLightBuffer() {
    LightUniformBuffer lightUbo{};
    lightUbo.numLights = 0;
    lightUbo.ambientIntensity = 0.15f;

    // Collect lights from scene
    if (m_scene) {
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto lightComp = actor->getComponent<LightComponent>();
            if (!lightComp) continue;

            if (lightUbo.numLights >= static_cast<int>(MAX_LIGHTS)) break;

            LightData& light = lightUbo.lights[lightUbo.numLights];

            // Get world position from actor's transform
            glm::vec3 worldPos = actor->getTransform()->getPosition();
            glm::vec3 worldDir = lightComp->getDirection();

            // Pack light type into position.w
            light.position = glm::vec4(worldPos, static_cast<float>(lightComp->getLightType()));

            // Pack range into direction.w
            light.direction = glm::vec4(worldDir, lightComp->getRange());

            // Pack intensity into color.w
            light.color = glm::vec4(lightComp->getColor(), lightComp->getIntensity());

            // Spot light params
            light.params = glm::vec4(
                glm::cos(glm::radians(lightComp->getInnerConeAngle())),
                glm::cos(glm::radians(lightComp->getOuterConeAngle())),
                0.0f, 0.0f
            );

            lightUbo.numLights++;
        }
    }

    // If no lights in scene, add a default directional light
    if (lightUbo.numLights == 0) {
        LightData& defaultLight = lightUbo.lights[0];
        defaultLight.position = glm::vec4(0.0f, 5.0f, 5.0f, 0.0f);  // type 0 = directional
        defaultLight.direction = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f)), 100.0f);
        defaultLight.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // white, intensity 1.0
        defaultLight.params = glm::vec4(0.0f);
        lightUbo.numLights = 1;
    }

    memcpy(m_lightBufferMapped, &lightUbo, sizeof(lightUbo));
}

} // namespace ohao
