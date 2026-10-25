#include "ssr_pass.hpp"
#include "gbuffer_pass.hpp"
#include "core/log.hpp"
#include <cstring>

namespace ohao {

namespace {

constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kFragment = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

} // namespace

SSRPass::~SSRPass() { cleanup(); }

bool SSRPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;

    m_sampler = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (m_sampler == VK_NULL_HANDLE || !createOutputImage()) return false;

    using rtc::layoutBinding;
    const VkDescriptorSetLayoutBinding bindings[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(4, VK_DESCRIPTOR_TYPE_SAMPLER),       layoutBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 6;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_descriptorLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_descriptorLayout);
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_descriptorPool;
    dai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &dai, m_descriptorSets.data()) != VK_SUCCESS) return false;

    if (!rtc::createComputePipeline(m_device, "postprocess_ssr.slang.spv", "trace", m_descriptorLayout,
                                    sizeof(gpu::shared::SSRPush), m_pipelineLayout, m_pipeline))
        return false;

    m_params.cameraPos = {0.0f, 0.0f, 0.0f, 20.0f};   // w: the march's length in metres
    m_params.params = {0.05f, 0.0f, 0.0f, 0.0f};      // x: thickness (NDC depth)
    OHAO_LOG(Info, render, "SSRPass: initialized ({}x{}, traced at {}x{})", m_width, m_height, m_output.width,
             m_output.height);
    return true;
}

bool SSRPass::createOutputImage() {
    return rtc::createImage(m_device, m_physicalDevice, m_output, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, (m_width + 1) / 2,
                            (m_height + 1) / 2);
}

void SSRPass::cleanup() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_sampler);
    rtc::destroyImage(m_device, m_output);
    m_descriptorSets = {};
}

void SSRPass::setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    std::memcpy(m_params.viewProj.data(), &viewProj[0][0], sizeof(float) * 16);
    m_params.cameraPos = {cameraPos.x, cameraPos.y, cameraPos.z, m_params.cameraPos[3]};
}

void SSRPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_pipeline || !m_gbuffer || !m_litSceneView || frameIndex >= MAX_FRAMES_IN_FLIGHT) return;
    const VkDescriptorSet set = m_descriptorSets[frameIndex];

    const VkDescriptorImageInfo infos[6] = {
        {VK_NULL_HANDLE, m_gbuffer->getPositionView(), kReadOnly},
        {VK_NULL_HANDLE, m_gbuffer->getNormalView(), kReadOnly},
        {VK_NULL_HANDLE, m_gbuffer->getDepthView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, m_litSceneView, kReadOnly},
        {m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
        {VK_NULL_HANDLE, m_output.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[6];
    for (uint32_t i = 0; i < 6; ++i) {
        const VkDescriptorType type = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                    : i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i] = rtc::descriptorWrite(set, i, type);
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);

    rtc::transition(cmd, m_output, VK_IMAGE_LAYOUT_GENERAL, kFragment, VK_ACCESS_SHADER_READ_BIT, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
    m_params.extent = {m_output.width, m_output.height, m_width, m_height};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_params), &m_params);
    vkCmdDispatch(cmd, (m_output.width + 7) / 8, (m_output.height + 7) / 8, 1);
    rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kFragment, VK_ACCESS_SHADER_READ_BIT,
                    false);
}

void SSRPass::onResize(uint32_t w, uint32_t h) {
    if (w == m_width && h == m_height) return;
    m_width = w;
    m_height = h;
    vkDeviceWaitIdle(m_device);
    rtc::destroyImage(m_device, m_output);
    if (!createOutputImage()) OHAO_LOG(Error, render, "SSRPass: failed to recreate its output at {}x{}", w, h);
}

} // namespace ohao
