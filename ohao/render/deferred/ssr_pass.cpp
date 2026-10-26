#include "ssr_pass.hpp"
#include "gbuffer_pass.hpp"
#include "core/log.hpp"
#include <cstring>

namespace ohao {

namespace {

constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

void litBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
                VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

SSRPass::~SSRPass() { cleanup(); }

bool SSRPass::createSetLayout(const VkDescriptorSetLayoutBinding* bindings, uint32_t count, VkDescriptorSetLayout& layout,
                              std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& sets) {
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = count;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &layout) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(layout);
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_descriptorPool;
    dai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    dai.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(m_device, &dai, sets.data()) == VK_SUCCESS;
}

bool SSRPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;

    m_sampler = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (m_sampler == VK_NULL_HANDLE || !createOutputImage()) return false;

    // The trace: 5 sampled images (the emissive target last), the sampler and its output; the
    // resolve: 4 sampled images
    // (the trace, position, the environment's specular, its weight) and the lit image.
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 9 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2 * MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;

    using rtc::layoutBinding;
    const VkDescriptorSetLayoutBinding trace[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(4, VK_DESCRIPTOR_TYPE_SAMPLER),       layoutBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        layoutBinding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)};
    const VkDescriptorSetLayoutBinding resolve[] = {
        layoutBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE), layoutBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        layoutBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)};
    if (!createSetLayout(trace, 7, m_descriptorLayout, m_descriptorSets) ||
        !createSetLayout(resolve, 5, m_resolveDescriptorLayout, m_resolveSets))
        return false;

    if (!rtc::createComputePipeline(m_device, "postprocess_ssr.slang.spv", "trace", m_descriptorLayout,
                                    sizeof(gpu::shared::SSRPush), m_pipelineLayout, m_pipeline) ||
        !rtc::createComputePipeline(m_device, "postprocess_ssr_resolve.slang.spv", "resolve", m_resolveDescriptorLayout,
                                    sizeof(gpu::shared::SSRResolvePush), m_resolvePipelineLayout, m_resolvePipeline))
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
    safeDestroy(m_resolvePipeline);
    safeDestroy(m_resolvePipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_resolveDescriptorLayout);
    safeDestroy(m_sampler);
    rtc::destroyImage(m_device, m_output);
    m_descriptorSets = {};
    m_resolveSets = {};
}

void SSRPass::setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    std::memcpy(m_params.viewProj.data(), &viewProj[0][0], sizeof(float) * 16);
    m_params.cameraPos = {cameraPos.x, cameraPos.y, cameraPos.z, m_params.cameraPos[3]};
    m_resolveParams.cameraPos = {cameraPos.x, cameraPos.y, cameraPos.z, 0.0f};
}

void SSRPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_pipeline || !m_resolvePipeline || !m_gbuffer || !m_litImage || !m_litSceneView || !m_envSpecularView ||
        !m_specularWeightView || frameIndex >= MAX_FRAMES_IN_FLIGHT)
        return;

    // The lighting pass's three attachments (and the sky's and particles' draws into the lit
    // image) are read here by compute.
    VkMemoryBarrier attachments{};
    attachments.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    attachments.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    attachments.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, kCompute, 0, 1, &attachments, 0, nullptr,
                         0, nullptr);

    // The trace, at half resolution.
    {
        const VkDescriptorSet set = m_descriptorSets[frameIndex];
        const VkDescriptorImageInfo infos[7] = {
            {VK_NULL_HANDLE, m_gbuffer->getPositionView(), kReadOnly},
            {VK_NULL_HANDLE, m_gbuffer->getNormalView(), kReadOnly},
            {VK_NULL_HANDLE, m_gbuffer->getDepthView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, m_litSceneView, kReadOnly},
            {m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
            {VK_NULL_HANDLE, m_output.view, kGeneral},
            {VK_NULL_HANDLE, m_gbuffer->getEmissiveView(), kReadOnly}};
        VkWriteDescriptorSet writes[7];
        for (uint32_t i = 0; i < 7; ++i) {
            const VkDescriptorType type = i < 4 || i == 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                        : i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i] = rtc::descriptorWrite(set, i, type);
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 7, writes, 0, nullptr);

        rtc::transition(cmd, m_output, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT,
                        true);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
        m_params.extent = {m_output.width, m_output.height, m_width, m_height};
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_params), &m_params);
        vkCmdDispatch(cmd, (m_output.width + 7) / 8, (m_output.height + 7) / 8, 1);
        rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT,
                        false);
    }

    // The resolve, at full resolution, in place on the lit image (to GENERAL and back, after
    // the trace's reads of it).
    litBarrier(cmd, m_litImage, kReadOnly, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT, kCompute,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    {
        const VkDescriptorSet set = m_resolveSets[frameIndex];
        const VkDescriptorImageInfo infos[5] = {{VK_NULL_HANDLE, m_output.view, kReadOnly},
                                                {VK_NULL_HANDLE, m_gbuffer->getPositionView(), kReadOnly},
                                                {VK_NULL_HANDLE, m_envSpecularView, kReadOnly},
                                                {VK_NULL_HANDLE, m_specularWeightView, kReadOnly},
                                                {VK_NULL_HANDLE, m_litSceneView, kGeneral}};
        VkWriteDescriptorSet writes[5];
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i] = rtc::descriptorWrite(set, i, i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipelineLayout, 0, 1, &set, 0, nullptr);
        m_resolveParams.extent = {m_width, m_height, m_output.width, m_output.height};
        vkCmdPushConstants(cmd, m_resolvePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(m_resolveParams),
                           &m_resolveParams);
        vkCmdDispatch(cmd, (m_width + 7) / 8, (m_height + 7) / 8, 1);
    }
    litBarrier(cmd, m_litImage, kGeneral, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
               VK_ACCESS_SHADER_READ_BIT);
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
