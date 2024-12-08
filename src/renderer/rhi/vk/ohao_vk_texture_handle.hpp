#pragma once
#include <vulkan/vulkan.h>
#include <cassert>

namespace ohao {

class OhaoVkTextureHandle {
public:
    explicit OhaoVkTextureHandle(VkDescriptorSet descriptorSet) : descriptorSet(descriptorSet) {}

    VkDescriptorSet getDescriptorSet() const { return descriptorSet; }
    operator bool() const { return descriptorSet != VK_NULL_HANDLE; }


private:
    VkDescriptorSet descriptorSet;
};

} // namespace ohao
