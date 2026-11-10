#include "sss_pass.hpp"
#include "render/shader_bindings/postprocess_sss.hpp"
#include "gbuffer_pass.hpp"
#include "gpu/shared_layout.hpp"
#include "core/log.hpp"
#include <tuple>
#include <utility>

namespace ohao {

namespace shared = gpu::shared;

namespace {

constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kGeneral = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

// Offsets of the two indirect dispatches in the classification buffer's header.
constexpr VkDeviceSize kVerticalArgs = 0;
constexpr VkDeviceSize kHorizontalArgs = 4 * sizeof(uint32_t);

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
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

SSSPass::~SSSPass() { cleanup(); }

bool SSSPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;
    if (!rtc::createImage(m_device, m_physicalDevice, m_scratch, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_width, m_height))
        return false;

    // 7, 8 (OpenPBR step 4): the G-buffer's layers target (each pixel's material) and the material
    // table (its subsurface's free path and colour). blurH's 0 is the subsurface diffuse light, blurV's
    // 9 the same image (the centre's own, which it takes out of the lit image).
    const VkDescriptorSetLayoutBinding h[] = {
        shader::layoutBinding(shader::postprocess_sss::hLit), shader::layoutBinding(shader::postprocess_sss::gPosition),
        shader::layoutBinding(shader::postprocess_sss::gNormal), shader::layoutBinding(shader::postprocess_sss::tiles),
        shader::layoutBinding(shader::postprocess_sss::hOut), shader::layoutBinding(shader::postprocess_sss::gLayers),
        shader::layoutBinding(shader::postprocess_sss::materials)};
    const VkDescriptorSetLayoutBinding v[] = {
        shader::layoutBinding(shader::postprocess_sss::gPosition), shader::layoutBinding(shader::postprocess_sss::gNormal),
        shader::layoutBinding(shader::postprocess_sss::tiles), shader::layoutBinding(shader::postprocess_sss::vIn),
        shader::layoutBinding(shader::postprocess_sss::vLit), shader::layoutBinding(shader::postprocess_sss::gLayers),
        shader::layoutBinding(shader::postprocess_sss::materials), shader::layoutBinding(shader::postprocess_sss::vDiffuse)};
    for (auto [bindings, count, layout] : {std::tuple{h, 7u, &m_hSetLayout}, std::tuple{v, 8u, &m_vSetLayout}}) {
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = count;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, layout) != VK_SUCCESS) return false;
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 9 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * MAX_FRAMES_IN_FLIGHT},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAX_FRAMES_IN_FLIGHT}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2 * MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS) return false;
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        for (auto [layout, set] : {std::pair{m_hSetLayout, &m_hSets[s]}, std::pair{m_vSetLayout, &m_vSets[s]}}) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = m_descriptorPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            if (vkAllocateDescriptorSets(m_device, &ai, set) != VK_SUCCESS) return false;
        }
    }
    if (!rtc::createComputePipeline(m_device, "postprocess_sss.slang.blurH.spv", "blurH", m_hSetLayout,
                                    sizeof(shared::SSSPush), m_hLayout, m_hPipeline) ||
        !rtc::createComputePipeline(m_device, "postprocess_sss.slang.blurV.spv", "blurV", m_vSetLayout,
                                    sizeof(shared::SSSPush), m_vLayout, m_vPipeline))
        return false;
    OHAO_LOG(Info, render, "SSSPass: initialized");
    return true;
}

void SSSPass::cleanup() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    safeDestroy(m_hPipeline);
    safeDestroy(m_vPipeline);
    safeDestroy(m_hLayout);
    safeDestroy(m_vLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_hSetLayout);
    safeDestroy(m_vSetLayout);
    rtc::destroyImage(m_device, m_scratch);
    m_hSets = {};
    m_vSets = {};
}

void SSSPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_hPipeline || !m_vPipeline || !m_gbuffer || !m_litImage || !m_diffuseView || frameIndex >= MAX_FRAMES_IN_FLIGHT)
        return;
    const VkBuffer tiles = m_gbuffer->sssTileBuffer();
    if (!tiles) return;

    // The G-buffer pass's fragments flagged the tiles: its writes before the indirect reads.
    VkBufferMemoryBarrier bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = tiles;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &bb, 0, nullptr);

    shared::SSSPush pc{};
    const VkExtent2D t = m_gbuffer->sssTiles();
    pc.extent = {m_width, m_height, t.width, t.height};
    // y: a pixel's world size at unit distance (2 tan(fov/2) / height), for the profile's radius.
    pc.params = {m_sssWidth, 2.0f / (std::abs(m_gbuffer->projection()[1][1]) * float(std::max(m_height, 1u))), 0.0f, 0.0f};
    const VkImageView layers = m_gbuffer->getLayersView();
    const VkBuffer materials = m_gbuffer->materialBuffer();
    if (!layers || !materials) return;
    const VkDescriptorImageInfo layersInfo{VK_NULL_HANDLE, layers, kReadOnly};
    const VkDescriptorBufferInfo materialInfo{materials, 0, VK_WHOLE_SIZE};
    pc.cameraPos = {m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 0.0f};
    const VkDescriptorBufferInfo tileInfo{tiles, 0, VK_WHOLE_SIZE};
    const VkImageView position = m_gbuffer->getPositionView(), normal = m_gbuffer->getNormalView();

    // Horizontal: the subsurface diffuse light -> scratch
    rtc::transition(cmd, m_scratch, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT, true);
    {
        const VkDescriptorSet set = m_hSets[frameIndex];
        const VkDescriptorImageInfo images[4] = {{VK_NULL_HANDLE, m_diffuseView, kReadOnly},
                                                 {VK_NULL_HANDLE, position, kReadOnly},
                                                 {VK_NULL_HANDLE, normal, kReadOnly},
                                                 {VK_NULL_HANDLE, m_scratch.view, kGeneral}};
        VkWriteDescriptorSet writes[7] = {
            shader::write(shader::postprocess_sss::hLit, set), shader::write(shader::postprocess_sss::gPosition, set),
            shader::write(shader::postprocess_sss::gNormal, set), shader::write(shader::postprocess_sss::tiles, set),
            shader::write(shader::postprocess_sss::hOut, set), shader::write(shader::postprocess_sss::gLayers, set),
            shader::write(shader::postprocess_sss::materials, set)};
        writes[0].pImageInfo = &images[0];
        writes[1].pImageInfo = &images[1];
        writes[2].pImageInfo = &images[2];
        writes[3].pBufferInfo = &tileInfo;
        writes[4].pImageInfo = &images[3];
        writes[5].pImageInfo = &layersInfo;
        writes[6].pBufferInfo = &materialInfo;
        vkUpdateDescriptorSets(m_device, 7, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_hLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatchIndirect(cmd, tiles, kHorizontalArgs);
    }

    // Vertical: scratch -> lit, in place (the lit image to GENERAL and back)
    const VkDescriptorImageInfo diffuseInfo{VK_NULL_HANDLE, m_diffuseView, kReadOnly};
    rtc::transition(cmd, m_scratch, kGeneral, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kCompute, VK_ACCESS_SHADER_READ_BIT, false);
    imageBarrier(cmd, m_litImage, kReadOnly, kGeneral, kCompute, VK_ACCESS_SHADER_READ_BIT, kCompute, VK_ACCESS_SHADER_WRITE_BIT);
    {
        const VkDescriptorSet set = m_vSets[frameIndex];
        const VkDescriptorImageInfo images[4] = {{VK_NULL_HANDLE, position, kReadOnly},
                                                 {VK_NULL_HANDLE, normal, kReadOnly},
                                                 {VK_NULL_HANDLE, m_scratch.view, kGeneral},
                                                 {VK_NULL_HANDLE, m_litView, kGeneral}};
        VkWriteDescriptorSet writes[8] = {
            shader::write(shader::postprocess_sss::gPosition, set), shader::write(shader::postprocess_sss::gNormal, set),
            shader::write(shader::postprocess_sss::tiles, set), shader::write(shader::postprocess_sss::vIn, set),
            shader::write(shader::postprocess_sss::vLit, set), shader::write(shader::postprocess_sss::gLayers, set),
            shader::write(shader::postprocess_sss::materials, set), shader::write(shader::postprocess_sss::vDiffuse, set)};
        writes[0].pImageInfo = &images[0];
        writes[1].pImageInfo = &images[1];
        writes[2].pBufferInfo = &tileInfo;
        writes[3].pImageInfo = &images[2];
        writes[4].pImageInfo = &images[3];
        writes[5].pImageInfo = &layersInfo;
        writes[6].pBufferInfo = &materialInfo;
        writes[7].pImageInfo = &diffuseInfo;
        vkUpdateDescriptorSets(m_device, 8, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_vPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_vLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_vLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatchIndirect(cmd, tiles, kVerticalArgs);
    }
    imageBarrier(cmd, m_litImage, kGeneral, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT);
}

void SSSPass::onResize(uint32_t w, uint32_t h) {
    if (w == m_width && h == m_height) return;
    m_width = w;
    m_height = h;
    vkDeviceWaitIdle(m_device);
    rtc::destroyImage(m_device, m_scratch);
    if (!rtc::createImage(m_device, m_physicalDevice, m_scratch, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_width, m_height))
        OHAO_LOG(Error, render, "SSSPass: failed to recreate its scratch image at {}x{}", w, h);
}

} // namespace ohao
