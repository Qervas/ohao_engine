#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include "ohao_vk_buffer.hpp"
#include <glm/glm.hpp>
#include "../../core/camera.hpp"

namespace ohao {

class OhaoVkDevice;

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 viewPos;
    float padding1;

    glm::vec3 lightPos;
    float padding2;
    glm::vec3 lightColor;
    float lightIntensity;

    glm::vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;
};

class OhaoVkUniformBuffer {
public:
    OhaoVkUniformBuffer() = default;
    ~OhaoVkUniformBuffer();

    bool initialize(OhaoVkDevice* device, uint32_t frameCount, VkDeviceSize bufferSize);
    void cleanup();

    // Buffer operations
    bool writeToBuffer(uint32_t frameIndex, const void* data, VkDeviceSize size);
    void* getMappedMemory(uint32_t frameIndex) const;
    OhaoVkBuffer* getBuffer(uint32_t frameIndex) const;

    const std::vector<std::unique_ptr<OhaoVkBuffer>>& getBuffers() const { return uniformBuffers; }
    uint32_t getBufferCount() const { return static_cast<uint32_t>(uniformBuffers.size()); }

    void updateFromCamera(uint32_t frameIndex, const Camera& camera);
    void setLightProperties(const glm::vec3& pos, const glm::vec3& color, float intensity);
    void setMaterialProperties(const glm::vec3& color, float metallic, float roughness, float ao);
    bool needsUpdating() const { return needsUpdate; }
    void markForUpdate() { needsUpdate = true; }
    void markAsUpdated() { needsUpdate = false; }

private:
    OhaoVkDevice* device{nullptr};
    std::vector<std::unique_ptr<OhaoVkBuffer>> uniformBuffers;
    std::vector<void*> mappedMemory;
    VkDeviceSize bufferSize{0};
    bool needsUpdate{true};
    UniformBufferObject cachedUBO{};


    bool createUniformBuffers(uint32_t frameCount, VkDeviceSize size);
};

} // namespace ohao
