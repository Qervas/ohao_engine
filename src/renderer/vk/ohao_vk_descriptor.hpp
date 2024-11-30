#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

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
    bool createDescriptorSets(const std::vector<VkBuffer>& uniformBuffers,
                             VkDeviceSize bufferSize);

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
