#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <vector>
#include "ohao_vk_buffer.hpp"

namespace ohao {

class OhaoVkDevice;

class OhaoVkDescriptor {
public:
    OhaoVkDescriptor() = default;
    ~OhaoVkDescriptor();

    bool initialize(OhaoVkDevice* device, uint32_t maxSets);
    void cleanup();

    // Layout management
    bool createSetLayout();

    // Pool and set management
    bool createPool();
    bool createDescriptorSets(const std::vector<std::unique_ptr<OhaoVkBuffer>>& uniformBuffers,
                             VkDeviceSize bufferSize);
    void updateDescriptorSet(
            uint32_t index,
            const OhaoVkBuffer& buffer,
            VkDeviceSize size,
            VkDeviceSize offset = 0);

    // Getters
    VkDescriptorSetLayout getLayout() const { return layout; }
    VkDescriptorPool getPool() const { return pool; }
    const VkDescriptorSet& getSet(uint32_t index) const { return descriptorSets[index]; }
    const std::vector<VkDescriptorSet>& getSets() const { return descriptorSets; }

private:
    OhaoVkDevice* device{nullptr};
    uint32_t maxSets{0};

    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    VkDescriptorPool pool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> descriptorSets;
};

} // namespace ohao
