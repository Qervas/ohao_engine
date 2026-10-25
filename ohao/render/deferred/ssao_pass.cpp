#include "ssao_pass.hpp"
#include "gpu/shared_layout.hpp"
#include "core/log.hpp"
#include <cstring>
#include <numeric>
#include <tuple>
#include <utility>

namespace ohao {

namespace shared = gpu::shared;

namespace {

constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

std::array<float, 4> f4(glm::vec3 v, float w) { return {v.x, v.y, v.z, w}; }

} // namespace

SSAOPass::~SSAOPass() {
    cleanup();
}

bool SSAOPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;
    if (!createImages()) return false;
    m_sampler = createSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (m_sampler == VK_NULL_HANDLE) return false;

    using rtc::layoutBinding;
    const VkDescriptorSetLayoutBinding occlusion[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    const VkDescriptorSetLayoutBinding blur[] = {layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                                 layoutBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    for (auto [bindings, count, layout] : {std::tuple{occlusion, 3u, &m_occlusionSetLayout}, std::tuple{blur, 2u, &m_blurSetLayout}}) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = count;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, layout) != VK_SUCCESS) return false;
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2 * MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        for (auto [layout, set] : {std::pair{m_occlusionSetLayout, &m_occlusionSets[s]}, std::pair{m_blurSetLayout, &m_blurSets[s]}}) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = m_descriptorPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            if (vkAllocateDescriptorSets(m_device, &ai, set) != VK_SUCCESS) return false;
        }
    }
    if (!rtc::createComputePipeline(m_device, "compute_ssao.slang.occlusion.spv", "occlusion", m_occlusionSetLayout,
                                    sizeof(shared::SSAOPush), m_occlusionLayout, m_occlusionPipeline) ||
        !rtc::createComputePipeline(m_device, "compute_ssao.slang.blur.spv", "blur", m_blurSetLayout,
                                    sizeof(shared::SSAOPush), m_blurLayout, m_blurPipeline)) {
        OHAO_LOG(Error, render, "SSAOPass: pipelines failed");
        return false;
    }
    return true;
}

bool SSAOPass::createImages() {
    const uint32_t w = (m_width + 1) / 2, h = (m_height + 1) / 2;
    return rtc::createImage(m_device, m_physicalDevice, m_occlusion, kOutputFormat, kUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            w, h) &&
           rtc::createImage(m_device, m_physicalDevice, m_output, kOutputFormat, kUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            w, h);
}

void SSAOPass::destroyImages() {
    rtc::destroyImage(m_device, m_occlusion);
    rtc::destroyImage(m_device, m_output);
}

void SSAOPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    safeDestroy(m_occlusionPipeline);
    safeDestroy(m_blurPipeline);
    safeDestroy(m_occlusionLayout);
    safeDestroy(m_blurLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_occlusionSetLayout);
    safeDestroy(m_blurSetLayout);
    safeDestroy(m_sampler);
    destroyImages();
    m_occlusionSets = {};
    m_blurSets = {};
}

void SSAOPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (m_positionView == VK_NULL_HANDLE || m_normalView == VK_NULL_HANDLE) return;
    if (!m_occlusionPipeline || !m_blurPipeline || frameIndex >= MAX_FRAMES_IN_FLIGHT) return;

    shared::SSAOPush pc{};
    std::memcpy(pc.viewProj.data(), &m_viewProj[0][0], sizeof(float) * 16);
    pc.cameraPosRadius = f4(m_cameraPos, m_radius);
    pc.params = {m_bias, m_intensity, m_falloffStart, m_falloffEnd};
    pc.extent = {m_occlusion.width, m_occlusion.height, m_width, m_height};
    pc.config = {m_directions, m_steps, 0u, 0u};
    const auto n = noise(m_noiseSeed);
    for (uint32_t i = 0; i < 4; ++i) {
        pc.noise0[i] = n[i];
        pc.noise1[i] = n[4 + i];
        pc.noise2[i] = n[8 + i];
        pc.noise3[i] = n[12 + i];
    }
    const uint32_t gx = (m_occlusion.width + 7) / 8, gy = (m_occlusion.height + 7) / 8;

    // Occlusion: G-buffer -> m_occlusion
    rtc::transition(cmd, m_occlusion, kGeneral, kReaders, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT, true);
    {
        const VkDescriptorSet set = m_occlusionSets[frameIndex];
        const VkDescriptorImageInfo infos[3] = {{VK_NULL_HANDLE, m_positionView, kReadOnly},
                                                {VK_NULL_HANDLE, m_normalView, kReadOnly},
                                                {VK_NULL_HANDLE, m_occlusion.view, kGeneral}};
        VkWriteDescriptorSet writes[3];
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i] = rtc::descriptorWrite(set, i, i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_occlusionPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_occlusionLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_occlusionLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);
    }

    // Blur: m_occlusion -> m_output
    rtc::transition(cmd, m_occlusion, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT, false);
    rtc::transition(cmd, m_output, kGeneral, kReaders, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT, true);
    {
        const VkDescriptorSet set = m_blurSets[frameIndex];
        const VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, m_occlusion.view, kReadOnly},
                                                {VK_NULL_HANDLE, m_output.view, kGeneral}};
        VkWriteDescriptorSet writes[2] = {rtc::descriptorWrite(set, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                          rtc::descriptorWrite(set, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
        writes[0].pImageInfo = &infos[0];
        writes[1].pImageInfo = &infos[1];
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_blurLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT, false);
}

void SSAOPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    vkDeviceWaitIdle(m_device);
    destroyImages();
    if (!createImages()) OHAO_LOG(Error, render, "SSAOPass: failed to recreate its images at {}x{}", width, height);
}

bool SSAOPass::reloadShader(std::string_view spvPath) {
    OHAO_LOG(Warn, render, "SSAOPass: {} not reloaded (two entry points: rebuild the shaders)", spvPath);
    return false;
}

// A seeded permutation of the 16 strata (k + 0.5) / 16: a counter hash (splitmix32-style)
// drives a Fisher-Yates shuffle, so the rotations are the same on every compiler and machine.
std::array<float, 16> SSAOPass::noise(uint32_t seed) {
    uint32_t state = seed;
    const auto next = [&state]() {
        uint32_t z = (state += 0x9E3779B9u);
        z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
        z = (z ^ (z >> 13)) * 0xC2B2AE35u;
        return z ^ (z >> 16);
    };
    std::array<uint32_t, 16> order;
    std::iota(order.begin(), order.end(), 0u);
    for (uint32_t i = 15; i > 0; --i) std::swap(order[i], order[next() % (i + 1)]);
    std::array<float, 16> out;
    for (uint32_t i = 0; i < 16; ++i) out[i] = (static_cast<float>(order[i]) + 0.5f) / 16.0f;
    return out;
}

} // namespace ohao
