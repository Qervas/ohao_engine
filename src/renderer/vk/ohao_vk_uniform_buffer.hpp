#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include "ohao_vk_buffer.hpp"

namespace ohao {

class OhaoVkDevice;

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

private:
    OhaoVkDevice* device{nullptr};
    std::vector<std::unique_ptr<OhaoVkBuffer>> uniformBuffers;
    std::vector<void*> mappedMemory;
    VkDeviceSize bufferSize{0};

    bool createUniformBuffers(uint32_t frameCount, VkDeviceSize size);
};

} // namespace ohao
