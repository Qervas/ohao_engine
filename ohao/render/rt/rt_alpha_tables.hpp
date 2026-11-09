#pragma once

// What an inline ray query needs to alpha-test a candidate triangle (sub-project 12, "Materials:
// alpha modes"): RenderScene's instance, material-reference, material, mesh, triangle-slot,
// index and vertex tables, as one descriptor set per frame slot (set 1 of rt/shadow_trace.slang
// and rt/gi_trace.slang; rt/alpha_test.slang declares it). The bindless textures are set 2.
//
// Binding 7 of the same set is the smoke proxy buffer (sub-project 12, step 4; rt/smoke_proxy.slang):
// this frame's density spheres from the RenderScene's VfxFrame, and whether the traces use them.
// It is written with the rest, after the slot's fence wait.

#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace ohao::render { class RenderScene; }

namespace ohao::rtc {

class AlphaTables {
public:
    [[nodiscard]] bool init(VkDevice device, VkPhysicalDevice physicalDevice);
    void destroy();

    [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return m_layout; }
    // Rewrites the slot's set from the scene's tables (after the slot's fence wait: the scene's
    // buffers can be replaced as they grow) and its smoke proxies (`smokeInGi`: whether the traces
    // attenuate through them; off, they write a count of 0 and the traces see no smoke). False when
    // the scene has none of the tables yet.
    [[nodiscard]] bool update(uint32_t slot, const render::RenderScene& scene, bool smokeInGi);
    [[nodiscard]] VkDescriptorSet set(uint32_t slot) const noexcept {
        return slot < m_sets.size() ? m_sets[slot] : VK_NULL_HANDLE;
    }

private:
    void writeSmoke(uint32_t slot, const render::RenderScene& scene, bool smokeInGi);

    static constexpr uint32_t kBindings = 8;         // 0-6 the scene's tables, 7 the smoke proxies (rt/alpha_test.slang)
    static constexpr uint32_t kSceneBindings = 7;
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physical{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
    std::array<HostBuffer, MAX_FRAMES_IN_FLIGHT> m_smoke{};   // binding 7, per slot (host-visible)
};

} // namespace ohao::rtc
