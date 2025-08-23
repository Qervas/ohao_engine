#include "ohao_vk_uniform_buffer.hpp"
#include "ohao_vk_device.hpp"
#include <iostream>

namespace ohao {

OhaoVkUniformBuffer::~OhaoVkUniformBuffer() {
    cleanup();
}

bool OhaoVkUniformBuffer::initialize(OhaoVkDevice* devicePtr, uint32_t frameCount, VkDeviceSize size) {
    device = devicePtr;
    bufferSize = size;
    return createUniformBuffers(frameCount, size);
}

void OhaoVkUniformBuffer::cleanup() {
    uniformBuffers.clear();
    mappedMemory.clear();
}

bool OhaoVkUniformBuffer::createUniformBuffers(uint32_t frameCount, VkDeviceSize size) {
    uniformBuffers.resize(frameCount);
    mappedMemory.resize(frameCount);

    for (size_t i = 0; i < frameCount; i++) {
        uniformBuffers[i] = std::make_unique<OhaoVkBuffer>();
        uniformBuffers[i]->initialize(device);

        if (!uniformBuffers[i]->create(
            size,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            std::cerr << "Failed to create uniform buffer " << i << std::endl;
            return false;
        }

        // Map the memory for the entire lifetime
        if (!uniformBuffers[i]->map()) {
            std::cerr << "Failed to map uniform buffer " << i << std::endl;
            return false;
        }
        mappedMemory[i] = uniformBuffers[i]->getMappedMemory();
    }

    return true;
}

bool OhaoVkUniformBuffer::writeToBuffer(uint32_t frameIndex, const void* data, VkDeviceSize size) {
    if (frameIndex >= uniformBuffers.size()) {
        std::cerr << "Frame index out of range" << std::endl;
        return false;
    }

    if (size > bufferSize) {
        std::cerr << "Write size exceeds buffer size" << std::endl;
        return false;
    }

    uniformBuffers[frameIndex]->writeToBuffer(data, size);
    return true;
}

void* OhaoVkUniformBuffer::getMappedMemory(uint32_t frameIndex) const {
    if (frameIndex >= mappedMemory.size()) {
        throw std::runtime_error("Frame index out of range");
    }
    return mappedMemory[frameIndex];
}

OhaoVkBuffer* OhaoVkUniformBuffer::getBuffer(uint32_t frameIndex) const {
    if (frameIndex >= uniformBuffers.size()) {
        throw std::runtime_error("Frame index out of range");
    }
    return uniformBuffers[frameIndex].get();
}

void OhaoVkUniformBuffer::updateFromCamera(uint32_t frameIndex, const Camera& camera) {
    // Always update camera-dependent properties
    cachedUBO.model = cachedUBO.model;
    cachedUBO.view = camera.getViewMatrix();
    cachedUBO.proj = camera.getProjectionMatrix();
    cachedUBO.viewPos = camera.getPosition();
    cachedUBO.proj[1][1] *= -1;

    // Ensure legacy light properties are zeroed if we have new lights
    if (cachedUBO.numLights > 0) {
        cachedUBO.lightPos = glm::vec3(0.0f);
        cachedUBO.lightColor = glm::vec3(0.0f);
        cachedUBO.lightIntensity = 0.0f;
    }

    // Always write the current cached UBO to the buffer
    // The lights and other properties should already be up-to-date in cachedUBO
    writeToBuffer(frameIndex, &cachedUBO, sizeof(UniformBufferObject));
    
    // Reset the update flag
    needsUpdate = false;
}

void OhaoVkUniformBuffer::setLightProperties(
    const glm::vec3& pos,
    const glm::vec3& color,
    float intensity){
    cachedUBO.lightPos = pos;
    cachedUBO.lightColor = color;
    cachedUBO.lightIntensity = intensity;
    needsUpdate = true;
}

void OhaoVkUniformBuffer::setMaterialProperties(
    const glm::vec3& color,
    float metallic,
    float roughness,
    float ao){
    cachedUBO.baseColor = color;
    cachedUBO.metallic = metallic;
    cachedUBO.roughness = roughness;
    cachedUBO.ao = ao;
    needsUpdate = true;
}

void OhaoVkUniformBuffer::setLights(const std::vector<RenderLight>& lights) {
    cachedUBO.numLights = std::min(static_cast<int>(lights.size()), MAX_LIGHTS);
    
    for (int i = 0; i < cachedUBO.numLights; ++i) {
        cachedUBO.lights[i] = lights[i];
    }
    
    // Clear unused slots
    for (int i = cachedUBO.numLights; i < MAX_LIGHTS; ++i) {
        cachedUBO.lights[i] = {};
    }
    
    needsUpdate = true;
}

void OhaoVkUniformBuffer::clearLights() {
    cachedUBO.numLights = 0;
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        cachedUBO.lights[i] = {};
    }
    needsUpdate = true;
}

void OhaoVkUniformBuffer::addLight(const RenderLight& light) {
    if (cachedUBO.numLights < MAX_LIGHTS) {
        cachedUBO.lights[cachedUBO.numLights] = light;
        cachedUBO.numLights++;
        needsUpdate = true;
    }
}

} // namespace ohao
