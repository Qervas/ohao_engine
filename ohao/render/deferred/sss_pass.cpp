#include "sss_pass.hpp"
#include "gbuffer_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

SSSPass::~SSSPass() { cleanup(); }

bool SSSPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920; m_height = 1080;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    if (!createImages()) return false;
    if (!createDescriptors()) return false;
    if (!createComputePipeline()) return false;

    std::cout << "SSSPass: Initialized" << std::endl;
    return true;
}

void SSSPass::cleanup() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    if (m_pipeline) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr); m_descriptorLayout = VK_NULL_HANDLE; }
    if (m_sampler) { vkDestroySampler(m_device, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }
    auto destroyImg = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        if (view) { vkDestroyImageView(m_device, view, nullptr); view = VK_NULL_HANDLE; }
        if (img) { vkDestroyImage(m_device, img, nullptr); img = VK_NULL_HANDLE; }
        if (mem) { vkFreeMemory(m_device, mem, nullptr); mem = VK_NULL_HANDLE; }
    };
    destroyImg(m_temp, m_tempMem, m_tempView);
    destroyImg(m_output, m_outputMem, m_outputView);
}

bool SSSPass::createImages() {
    auto createImg = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ii.extent = {m_width, m_height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(m_device, &ii, nullptr, &img) != VK_SUCCESS) return false;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(m_device, img, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &ai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, img, mem, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(m_device, &vi, nullptr, &view) == VK_SUCCESS;
    };
    return createImg(m_temp, m_tempMem, m_tempView) &&
           createImg(m_output, m_outputMem, m_outputView);
}

bool SSSPass::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},  // 4 per pass × 2 passes
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayout layouts[2] = {m_descriptorLayout, m_descriptorLayout};
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_descriptorPool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    VkDescriptorSet sets[2];
    if (vkAllocateDescriptorSets(m_device, &dai, sets) != VK_SUCCESS) return false;
    m_descSetH = sets[0];
    m_descSetV = sets[1];
    return true;
}

bool SSSPass::createComputePipeline() {
    VkShaderModule shader = loadShaderModule("postprocess_sss_blur.comp.spv");
    if (!shader) { std::cerr << "SSSPass: shader not found" << std::endl; return false; }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = sizeof(SSSPushConstants);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &m_descriptorLayout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(m_device, &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, shader, nullptr); return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader; stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage; ci.layout = m_pipelineLayout;
    VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, shader, nullptr);
    return r == VK_SUCCESS;
}

void SSSPass::executePass(VkCommandBuffer cmd, VkImageView input, VkImage inputImg,
                           VkImageView output, VkImage outputImg, glm::vec2 direction) {
    if (!m_gbuffer) return;

    // Choose descriptor set based on direction
    VkDescriptorSet ds = (direction.x > 0.5f) ? m_descSetH : m_descSetV;

    // Update descriptors
    VkDescriptorImageInfo imgInfos[4] = {};
    imgInfos[0] = {m_sampler, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[1] = {m_sampler, m_gbuffer->getPositionView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[2] = {m_sampler, m_gbuffer->getNormalView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[3] = {m_sampler, m_gbuffer->getAlbedoView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo outInfo = {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[5] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = ds; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = ds; writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    // Transition output to GENERAL
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.image = outputImg;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &ds, 0, nullptr);
    SSSPushConstants pc{};
    pc.direction = direction;
    pc.screenSize = glm::vec2(m_width, m_height);
    pc.sssWidth = m_sssWidth;
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SSSPushConstants), &pc);
    vkCmdDispatch(cmd, (m_width + 15) / 16, (m_height + 15) / 16, 1);

    // Transition output to SHADER_READ for next pass
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = outputImg;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);
}

void SSSPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_pipeline || !m_gbuffer || !m_litSceneView) return;

    // Pass 1: Horizontal blur (lit scene → temp)
    executePass(cmd, m_litSceneView, VK_NULL_HANDLE, m_tempView, m_temp, glm::vec2(1, 0));

    // Pass 2: Vertical blur (temp → output)
    executePass(cmd, m_tempView, m_temp, m_outputView, m_output, glm::vec2(0, 1));
}

void SSSPass::onResize(uint32_t w, uint32_t h) {
    if (w == m_width && h == m_height) return;
    m_width = w; m_height = h;
    vkDeviceWaitIdle(m_device);
    auto destroyImg = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        if (view) vkDestroyImageView(m_device, view, nullptr);
        if (img) vkDestroyImage(m_device, img, nullptr);
        if (mem) vkFreeMemory(m_device, mem, nullptr);
        view = VK_NULL_HANDLE; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    };
    destroyImg(m_temp, m_tempMem, m_tempView);
    destroyImg(m_output, m_outputMem, m_outputView);
    createImages();
}

} // namespace ohao
