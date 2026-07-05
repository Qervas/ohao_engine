#include "ssr_pass.hpp"
#include "gbuffer_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

SSRPass::~SSRPass() { cleanup(); }

bool SSRPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;

    // Sampler
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    if (!createOutputImage()) return false;
    if (!createDescriptors()) return false;
    if (!createComputePipeline()) return false;

    m_params.maxDistance = 20.0f;
    m_params.thickness = 0.05f;

    std::cout << "SSRPass: Initialized (" << m_width << "x" << m_height << ")" << std::endl;
    return true;
}

void SSRPass::cleanup() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    if (m_pipeline) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr); m_descriptorLayout = VK_NULL_HANDLE; }
    if (m_sampler) { vkDestroySampler(m_device, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }
    if (m_outputView) { vkDestroyImageView(m_device, m_outputView, nullptr); m_outputView = VK_NULL_HANDLE; }
    if (m_output) { vkDestroyImage(m_device, m_output, nullptr); m_output = VK_NULL_HANDLE; }
    if (m_outputMem) { vkFreeMemory(m_device, m_outputMem, nullptr); m_outputMem = VK_NULL_HANDLE; }
}

bool SSRPass::createOutputImage() {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {m_width, m_height, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ii, nullptr, &m_output) != VK_SUCCESS) return false;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(m_device, m_output, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &ai, nullptr, &m_outputMem) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_output, m_outputMem, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_output;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(m_device, &vi, nullptr, &m_outputView) == VK_SUCCESS;
}

bool SSRPass::createDescriptors() {
    // 5 bindings: position, normal, depth, lit scene (samplers) + output (storage image)
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
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = m_descriptorPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &m_descriptorLayout;
    return vkAllocateDescriptorSets(m_device, &dai, &m_descriptorSet) == VK_SUCCESS;
}

bool SSRPass::createComputePipeline() {
    VkShaderModule shader = loadShaderModule("postprocess_ssr.comp.spv");
    if (shader == VK_NULL_HANDLE) {
        std::cerr << "SSRPass: Failed to load ssr.comp.spv" << std::endl;
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = sizeof(SSRPushConstants);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_descriptorLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(m_device, &pli, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, shader, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = m_pipelineLayout;

    VkResult r = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, shader, nullptr);
    return r == VK_SUCCESS;
}

void SSRPass::setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    m_params.viewProj = viewProj;
    m_params.invViewProj = glm::inverse(viewProj);
    m_params.cameraPos = glm::vec4(cameraPos, 0.0f);
    m_params.screenSize = glm::vec2(m_width, m_height);
}

void SSRPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_pipeline || !m_gbuffer || !m_litSceneView) return;

    // Update descriptors
    VkDescriptorImageInfo imgInfos[4] = {};
    imgInfos[0] = {m_sampler, m_gbuffer->getPositionView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[1] = {m_sampler, m_gbuffer->getNormalView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgInfos[2] = {m_sampler, m_gbuffer->getDepthView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    imgInfos[3] = {m_sampler, m_litSceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkDescriptorImageInfo outInfo = {VK_NULL_HANDLE, m_outputView, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[5] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imgInfos[i];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
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
    toGeneral.image = m_output;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SSRPushConstants), &m_params);
    vkCmdDispatch(cmd, (m_width + 15) / 16, (m_height + 15) / 16, 1);

    // Transition output to SHADER_READ
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = m_output;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toRead);
}

void SSRPass::onResize(uint32_t w, uint32_t h) {
    if (w == m_width && h == m_height) return;
    m_width = w; m_height = h;
    vkDeviceWaitIdle(m_device);
    if (m_outputView) vkDestroyImageView(m_device, m_outputView, nullptr);
    if (m_output) vkDestroyImage(m_device, m_output, nullptr);
    if (m_outputMem) vkFreeMemory(m_device, m_outputMem, nullptr);
    createOutputImage();
}

} // namespace ohao
