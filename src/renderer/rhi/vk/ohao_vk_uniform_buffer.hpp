#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include "ohao_vk_buffer.hpp"
#include <glm/glm.hpp>
#include "renderer/camera/camera.hpp"
#include "renderer/shader/shader_uniforms.hpp"

namespace ohao {

class OhaoVkDevice;

class OhaoVkUniformBuffer {
public:
    using UniformBufferObject =  GlobalUniformBuffer;

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
    void setLights(const std::vector<RenderLight>& lights);
    void clearLights();
    void addLight(const RenderLight& light);
    bool needsUpdating() const { return needsUpdate; }
    void markForUpdate() { needsUpdate = true; }
    void markAsUpdated() { needsUpdate = false; }
    const UniformBufferObject& getCachedUBO() const { return cachedUBO; }
    UniformBufferObject& getCachedUBO() { return cachedUBO; }
    void setCachedUBO(const UniformBufferObject& ubo) { cachedUBO = ubo; }
    void update(uint32_t frameIndex){writeToBuffer(frameIndex, &cachedUBO, sizeof(UniformBufferObject));}

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
