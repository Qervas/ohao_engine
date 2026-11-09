#pragma once
// What tools/gen_bindings.py writes for each shader (ohao/render/shader_bindings/<shader>.hpp), and the two things
// C++ does with it: make a descriptor set layout from a set's bindings, and write a descriptor at a binding.
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ohao::shader {

struct Binding {
    uint32_t set;
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;   // 0: an unsized array
};

// A set's layout bindings, each visible to `stages`.
[[nodiscard]] inline std::vector<VkDescriptorSetLayoutBinding> layoutBindings(std::span<const Binding> set,
                                                                              VkShaderStageFlags stages) {
    std::vector<VkDescriptorSetLayoutBinding> out;
    out.reserve(set.size());
    for (const Binding& b : set) out.push_back({b.binding, b.type, b.count, stages, nullptr});
    return out;
}

// A descriptor write of `b` into `set` (fill pImageInfo, pBufferInfo or pNext).
[[nodiscard]] inline VkWriteDescriptorSet write(const Binding& b, VkDescriptorSet set, uint32_t count = 1,
                                                uint32_t arrayElement = 0) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = b.binding;
    w.dstArrayElement = arrayElement;
    w.descriptorCount = count;
    w.descriptorType = b.type;
    return w;
}

} // namespace ohao::shader
