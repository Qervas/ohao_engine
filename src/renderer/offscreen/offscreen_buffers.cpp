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

void OffscreenRenderer::updateUniformBuffer(uint32_t frameIndex) {
    if (!m_frameResources.isInitialized()) {
        updateUniformBuffer();
        return;
    }

    FrameResources& frame = m_frameResources.getFrame(frameIndex);
    if (!frame.cameraBufferMapped) return;

    CameraUniformBuffer ubo{};
    ubo.view = m_camera->getViewMatrix();
    ubo.proj = glm::perspective(glm::radians(45.0f),
                                static_cast<float>(m_width) / static_cast<float>(m_height),
                                0.1f, 100.0f);
    // Flip Y for Vulkan
    ubo.proj[1][1] *= -1;
    ubo.viewPos = m_camera->getPosition();

    memcpy(frame.cameraBufferMapped, &ubo, sizeof(ubo));
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

glm::mat4 OffscreenRenderer::calculateLightSpaceMatrix(const LightData& light) {
    // For directional lights, use orthographic projection
    int lightType = static_cast<int>(light.position.w);

    if (lightType == 0) {  // Directional light
        glm::vec3 lightDir = glm::normalize(glm::vec3(light.direction));

        // Calculate scene bounds - use larger frustum to encompass typical scenes
        float orthoSize = 50.0f;  // Covers -50 to 50 units (increased from 25)
        float nearPlane = 0.1f;   // Closer near plane
        float farPlane = 200.0f;  // Further far plane

        // Light view matrix: looking from "infinity" in light direction toward scene center
        glm::vec3 sceneCenter = glm::vec3(0.0f, 0.0f, 0.0f);
        glm::vec3 lightPos = sceneCenter - lightDir * 100.0f;  // Position light further away

        // Calculate up vector (avoid parallel with light direction)
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, up);
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);

        // Flip Y for Vulkan coordinate system (do this BEFORE combining)
        lightProj[1][1] *= -1.0f;

        // Combine view and projection
        glm::mat4 lightSpaceMatrix = lightProj * lightView;

        // Convert Z from OpenGL NDC [-1,1] to Vulkan [0,1] range
        // Formula: z_vulkan = z_ndc * 0.5 + 0.5
        // This scales the Z output and adds a bias
        // In GLM column-major: matrix[col][row], so row 2 is the Z output
        for (int i = 0; i < 4; i++) {
            lightSpaceMatrix[i][2] *= 0.5f;  // Scale Z coefficients
        }
        lightSpaceMatrix[3][2] += 0.5f;  // Add bias (multiplied by w=1)

        return lightSpaceMatrix;
    }
    else if (lightType == 2) {  // Spot light - use perspective projection
        glm::vec3 lightPos = glm::vec3(light.position);
        glm::vec3 lightDir = glm::normalize(glm::vec3(light.direction));

        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        float outerCone = glm::acos(light.params.y);  // Convert from cosine to angle
        float fov = outerCone * 2.0f;
        float range = light.direction.w;

        glm::mat4 lightView = glm::lookAt(lightPos, lightPos + lightDir, up);
        float near = 0.1f;
        float far = range;
        glm::mat4 lightProj = glm::perspective(fov, 1.0f, near, far);

        // Flip Y for Vulkan coordinate system
        lightProj[1][1] *= -1.0f;

        // Combine view and projection
        glm::mat4 lightSpaceMatrix = lightProj * lightView;

        // Convert Z from OpenGL NDC [-1,1] to Vulkan [0,1] range
        for (int i = 0; i < 4; i++) {
            lightSpaceMatrix[i][2] *= 0.5f;  // Scale Z coefficients
        }
        lightSpaceMatrix[3][2] += 0.5f;  // Add bias (multiplied by w)

        return lightSpaceMatrix;
    }

    // Point lights don't use a single matrix (need cube maps)
    return glm::mat4(1.0f);
}

void OffscreenRenderer::updateLightBuffer() {
    LightUniformBuffer lightUbo{};
    lightUbo.numLights = 0;
    lightUbo.ambientIntensity = 0.15f;
    lightUbo.shadowBias = 0.005f;
    lightUbo.shadowStrength = 0.7f;

    int shadowCasterIndex = -1;  // Index of first shadow-casting light

    // Debug: Print scene state
    static int debugCounter = 0;
    bool shouldPrintDebug = (debugCounter++ % 120 == 0);  // Print every ~2 seconds at 60fps

    // Collect lights from scene
    if (m_scene) {
        size_t actorCount = m_scene->getAllActors().size();
        if (shouldPrintDebug) {
            std::cout << "[LightBuffer] Scene ptr=" << static_cast<void*>(m_scene)
                      << " has " << actorCount << " actors" << std::endl;
        }

        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto lightComp = actor->getComponent<LightComponent>();
            if (!lightComp) {
                if (shouldPrintDebug) {
                    std::cout << "  Actor '" << actor->getName() << "' - no LightComponent" << std::endl;
                }
                continue;
            }

            if (shouldPrintDebug) {
                std::cout << "  FOUND LIGHT: '" << actor->getName() << "' type="
                          << static_cast<int>(lightComp->getLightType())
                          << " pos=(" << actor->getTransform()->getPosition().x
                          << "," << actor->getTransform()->getPosition().y
                          << "," << actor->getTransform()->getPosition().z << ")"
                          << " dir=(" << lightComp->getDirection().x
                          << "," << lightComp->getDirection().y
                          << "," << lightComp->getDirection().z << ")"
                          << std::endl;
            }

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

            // Spot light params + shadow map index
            // Only first directional or spot light casts shadows for now
            float shadowMapIndex = -1.0f;
            if (m_shadowsEnabled && shadowCasterIndex < 0) {
                int lightType = static_cast<int>(lightComp->getLightType());
                if (lightType == 0 || lightType == 2) {  // Directional or Spot
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

            // Calculate light space matrix for shadow casting lights
            if (shadowMapIndex >= 0.0f) {
                light.lightSpaceMatrix = calculateLightSpaceMatrix(light);
                if (shouldPrintDebug) {
                    std::cout << "  Light space matrix [0]: ("
                              << light.lightSpaceMatrix[0][0] << ", "
                              << light.lightSpaceMatrix[0][1] << ", "
                              << light.lightSpaceMatrix[0][2] << ", "
                              << light.lightSpaceMatrix[0][3] << ")" << std::endl;
                    std::cout << "  Light space matrix [1]: ("
                              << light.lightSpaceMatrix[1][0] << ", "
                              << light.lightSpaceMatrix[1][1] << ", "
                              << light.lightSpaceMatrix[1][2] << ", "
                              << light.lightSpaceMatrix[1][3] << ")" << std::endl;
                    std::cout << "  Light space matrix [2]: ("
                              << light.lightSpaceMatrix[2][0] << ", "
                              << light.lightSpaceMatrix[2][1] << ", "
                              << light.lightSpaceMatrix[2][2] << ", "
                              << light.lightSpaceMatrix[2][3] << ")" << std::endl;
                    std::cout << "  Light space matrix [3]: ("
                              << light.lightSpaceMatrix[3][0] << ", "
                              << light.lightSpaceMatrix[3][1] << ", "
                              << light.lightSpaceMatrix[3][2] << ", "
                              << light.lightSpaceMatrix[3][3] << ")" << std::endl;
                }
            } else {
                light.lightSpaceMatrix = glm::mat4(1.0f);
            }

            lightUbo.numLights++;
        }
    }

    // If no lights in scene, add a default directional light with shadows
    if (lightUbo.numLights == 0) {
        if (shouldPrintDebug) {
            std::cout << "[LightBuffer] WARNING: No lights found in scene, using HARDCODED default!" << std::endl;
        }
        LightData& defaultLight = lightUbo.lights[0];
        defaultLight.position = glm::vec4(0.0f, 5.0f, 5.0f, 0.0f);  // type 0 = directional
        defaultLight.direction = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f)), 100.0f);
        defaultLight.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // white, intensity 1.0
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

void OffscreenRenderer::updateLightBuffer(uint32_t frameIndex) {
    if (!m_frameResources.isInitialized()) {
        updateLightBuffer();
        return;
    }

    FrameResources& frame = m_frameResources.getFrame(frameIndex);
    if (!frame.lightBufferMapped) return;

    LightUniformBuffer lightUbo{};
    lightUbo.numLights = 0;
    lightUbo.ambientIntensity = 0.15f;
    lightUbo.shadowBias = 0.005f;
    lightUbo.shadowStrength = 0.7f;

    int shadowCasterIndex = -1;

    // Collect lights from scene
    if (m_scene) {
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto lightComp = actor->getComponent<LightComponent>();
            if (!lightComp) continue;
            if (lightUbo.numLights >= static_cast<int>(MAX_LIGHTS)) break;

            LightData& light = lightUbo.lights[lightUbo.numLights];

            glm::vec3 worldPos = actor->getTransform()->getPosition();
            glm::vec3 worldDir = lightComp->getDirection();

            light.position = glm::vec4(worldPos, static_cast<float>(lightComp->getLightType()));
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

    // If no lights in scene, add a default directional light
    if (lightUbo.numLights == 0) {
        LightData& defaultLight = lightUbo.lights[0];
        defaultLight.position = glm::vec4(0.0f, 5.0f, 5.0f, 0.0f);
        defaultLight.direction = glm::vec4(glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f)), 100.0f);
        defaultLight.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        defaultLight.params = glm::vec4(0.0f, 0.0f, m_shadowsEnabled ? 0.0f : -1.0f, 0.0f);

        if (m_shadowsEnabled) {
            defaultLight.lightSpaceMatrix = calculateLightSpaceMatrix(defaultLight);
        } else {
            defaultLight.lightSpaceMatrix = glm::mat4(1.0f);
        }

        lightUbo.numLights = 1;
    }

    memcpy(frame.lightBufferMapped, &lightUbo, sizeof(lightUbo));
}

} // namespace ohao
