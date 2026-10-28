#pragma once

// What an inline ray query needs to alpha-test a candidate triangle (sub-project 12, "Materials:
// alpha modes"): RenderScene's instance, material-reference, material, mesh, triangle-slot,
// index and vertex tables, as one descriptor set per frame slot (set 1 of rt/shadow_trace.slang
// and rt/gi_trace.slang; rt/alpha_test.slang declares it). The bindless textures are set 2.

#include "render/frame/frame_resources.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace ohao::render { class RenderScene; }

namespace ohao::rtc {

class AlphaTables {
public:
    [[nodiscard]] bool init(VkDevice device);
    void destroy();

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    // Rewrites the slot's set from the scene's tables (after the slot's fence wait: the scene's
    // buffers can be replaced as they grow). False when the scene has none of them yet.
    [[nodiscard]] bool update(uint32_t slot, const render::RenderScene& scene);
    [[nodiscard]] VkDescriptorSet set(uint32_t slot) const noexcept {
        return slot < m_sets.size() ? m_sets[slot] : VK_NULL_HANDLE;
    }

private:
    static constexpr uint32_t kBindings = 7;
    VkDevice m_device{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
};

} // namespace ohao::rtc
