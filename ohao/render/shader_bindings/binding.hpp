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
    constexpr bool operator==(const Binding&) const = default;
};

// A set's layout bindings, each visible to `stages`.
[[nodiscard]] inline std::vector<VkDescriptorSetLayoutBinding> layoutBindings(std::span<const Binding> set,
                                                                              VkShaderStageFlags stages) {
    std::vector<VkDescriptorSetLayoutBinding> out;
    out.reserve(set.size());
    for (const Binding& b : set) out.push_back({b.binding, b.type, b.count, stages, nullptr});
    return out;
}

// One layout binding of `b` (a set's layout made a binding at a time).
[[nodiscard]] inline VkDescriptorSetLayoutBinding layoutBinding(const Binding& b,
                                                                VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT) {
    return {b.binding, b.type, b.count, stages, nullptr};
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

// Holds a hand-written C++ push-constant struct `T` to shader `ns`'s (a generated header's OHAO_PUSH_FIELDS_<ns>):
// every field the shader has must be in T, under the same name, at the same offset, and T no larger than the
// shader's block.
#define OHAO_CHECK_PUSH_FIELD(T, field, offset) \
    static_assert(offsetof(T, field) == (offset), #T "." #field " is not where the shader has it");
#define OHAO_CHECK_PUSH(T, ns)                                   \
    OHAO_PUSH_FIELDS_##ns(T, OHAO_CHECK_PUSH_FIELD)              \
    static_assert(sizeof(T) <= ::ohao::shader::ns::kPushSize, #T " is larger than the shader's push constants");
