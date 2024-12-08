#pragma once
#include "renderer/rhi/vk/ohao_vk_texture_handle.hpp"
#include "imgui.h"
#include <cassert>

namespace ohao {
namespace imgui {

inline ImTextureID convertVulkanTextureToImGui(const OhaoVkTextureHandle& textureHandle) {
    VkDescriptorSet descriptorSet = textureHandle.getDescriptorSet();
    assert(descriptorSet != VK_NULL_HANDLE && "Cannot convert null descriptor set to ImTextureID");

    // Some Vulkan implementations might have different descriptor set handle sizes
    static_assert(sizeof(ImTextureID) >= sizeof(VkDescriptorSet),
                 "ImTextureID cannot safely hold VkDescriptorSet");

    return static_cast<ImTextureID>(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(descriptorSet)));
}

} // namespace imgui
} // namespace ohao
