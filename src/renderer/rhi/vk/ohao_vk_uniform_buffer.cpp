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
    cachedUBO.model = glm::mat4(1.0f);
    cachedUBO.view = camera.getViewMatrix();
    cachedUBO.proj = camera.getProjectionMatrix();
    cachedUBO.viewPos = camera.getPosition();
    cachedUBO.proj[1][1] *= -1;

    // Only copy other properties if this is the first update or they've changed
    if (needsUpdate) {
        UniformBufferObject* currentUBO = static_cast<UniformBufferObject*>(getMappedMemory(frameIndex));
        cachedUBO.lightPos = currentUBO->lightPos;
        cachedUBO.lightColor = currentUBO->lightColor;
        cachedUBO.lightIntensity = currentUBO->lightIntensity;
        cachedUBO.baseColor = currentUBO->baseColor;
        cachedUBO.metallic = currentUBO->metallic;
        cachedUBO.roughness = currentUBO->roughness;
        cachedUBO.ao = currentUBO->ao;
        needsUpdate = false;
    }

    writeToBuffer(frameIndex, &cachedUBO, sizeof(UniformBufferObject));
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

} // namespace ohao
