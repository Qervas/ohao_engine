// AtrousDenoiser — Vulkan compute pipeline that runs a 5x5 B3-spline à-trous
// (SVGF-style) edge-aware filter over the RT beauty image. Denoises the final
// LDR PBR image directly (never demodulates), so metals/emissive stay correct.

#include "render/rt/denoise/atrous_denoise.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <vector>

namespace ohao {

namespace {

// Number of à-trous iterations. Step sizes double each pass: 1,2,4,8,16.
constexpr uint32_t kNumIterations = 5;

// Edge-stopping sensitivities. Tuned for the LDR RGBA8 beauty ([0,1] color),
// world normals, and linear view-space depth (world units).
constexpr float kSigmaColor  = 0.6f;
constexpr float kSigmaNormal = 0.30f;
constexpr float kSigmaDepth  = 2.0f;

struct AtrousPush {
    int   stepSize;
    float sigmaColor;
    float sigmaNormal;
    float sigmaDepth;
};

// Matches PathTracer / NrdCompositor readFile helper.
std::vector<char> readShaderSpv(const std::string& name) {
    const std::string searchPaths[] = {
        name,
        "build/shaders/" + name,
        "build/Release/bin/shaders/" + name,
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = static_cast<size_t>(file.tellg());
            std::vector<char> buf(size);
            file.seekg(0);
            file.read(buf.data(), size);
            return buf;
        }
    }
    return {};
}

}  // anonymous namespace

struct AtrousDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSets[kNumIterations] = {};

    // Two ping-pong scratch images (RGBA8, same size as beauty).
    VkImage        scratchImage[2]  = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory scratchMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView    scratchView[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool           scratchInitialized = false;  // first-frame UNDEFINED->GENERAL latch

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeFilter & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool createScratch(uint32_t idx) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = {width, height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imageInfo, nullptr, &scratchImage[idx]) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, scratchImage[idx], &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &scratchMemory[idx]) != VK_SUCCESS) return false;
        vkBindImageMemory(device, scratchImage[idx], scratchMemory[idx], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = scratchImage[idx];
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &scratchView[idx]) != VK_SUCCESS) return false;
        return true;
    }
};

AtrousDenoiser::AtrousDenoiser()  : m_impl(std::make_unique<Impl>()) {}
AtrousDenoiser::~AtrousDenoiser() { shutdown(); }

bool AtrousDenoiser::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // 1. Descriptor set layout: 4 storage-image bindings, COMPUTE stage.
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout) != VK_SUCCESS) {
        std::cerr << "[atrous] vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    // 2. Pipeline layout with push constants.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(AtrousPush);
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_impl->setLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[atrous] vkCreatePipelineLayout failed\n";
        return false;
    }

    // 3. Load SPV. shaders/rt/rt_atrous.comp -> build/shaders/rt_rt_atrous.comp.spv.
    auto spv = readShaderSpv("rt_rt_atrous.comp.spv");
    if (spv.empty()) {
        std::cerr << "[atrous] failed to load rt_rt_atrous.comp.spv\n";
        return false;
    }
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[atrous] vkCreateShaderModule failed\n";
        return false;
    }

    // 4. Compute pipeline.
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName  = "main";
    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage  = stage;
    pipeInfo.layout = m_impl->pipelineLayout;
    VkResult pipeResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                                   &m_impl->pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (pipeResult != VK_SUCCESS) {
        std::cerr << "[atrous] vkCreateComputePipelines failed\n";
        return false;
    }

    // 5. Descriptor pool: kNumIterations sets, 4 storage images each.
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = kNumIterations * 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = kNumIterations;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool) != VK_SUCCESS) {
        std::cerr << "[atrous] vkCreateDescriptorPool failed\n";
        return false;
    }
    std::array<VkDescriptorSetLayout, kNumIterations> layouts;
    layouts.fill(m_impl->setLayout);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = kNumIterations;
    dsai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(device, &dsai, m_impl->descriptorSets) != VK_SUCCESS) {
        std::cerr << "[atrous] vkAllocateDescriptorSets failed\n";
        return false;
    }

    // 6. Two ping-pong scratch images.
    if (!m_impl->createScratch(0) || !m_impl->createScratch(1)) {
        std::cerr << "[atrous] scratch image allocation failed\n";
        return false;
    }

    std::cout << "[atrous] pipeline ready @ " << width << "x" << height
              << " (" << kNumIterations << " iterations)\n";
    return true;
}

void AtrousDenoiser::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    for (int i = 0; i < 2; ++i) {
        if (m_impl->scratchView[i])   { vkDestroyImageView(d, m_impl->scratchView[i], nullptr);  m_impl->scratchView[i]  = VK_NULL_HANDLE; }
        if (m_impl->scratchImage[i])  { vkDestroyImage(d, m_impl->scratchImage[i], nullptr);      m_impl->scratchImage[i] = VK_NULL_HANDLE; }
        if (m_impl->scratchMemory[i]) { vkFreeMemory(d, m_impl->scratchMemory[i], nullptr);       m_impl->scratchMemory[i]= VK_NULL_HANDLE; }
    }
    if (m_impl->descriptorPool) { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)       { vkDestroyPipeline(d, m_impl->pipeline, nullptr);             m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout) { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr); m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)      { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr); m_impl->setLayout      = VK_NULL_HANDLE; }
    for (auto& s : m_impl->descriptorSets) s = VK_NULL_HANDLE;
    m_impl->device = VK_NULL_HANDLE;
}

void AtrousDenoiser::dispatch(VkCommandBuffer cmd, const AtrousInputs& inputs) {
    if (!m_impl->pipeline) return;
    Impl& I = *m_impl;

    // --- Transition both scratch images to GENERAL. First frame ever:
    //     UNDEFINED->GENERAL; afterwards GENERAL->GENERAL. Each à-trous pass
    //     fully overwrites its target, so no cross-frame data dependency. ---
    {
        VkImageMemoryBarrier b[2] = {};
        for (int i = 0; i < 2; ++i) {
            b[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].srcAccessMask    = I.scratchInitialized ? VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT : 0;
            b[i].dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b[i].oldLayout        = I.scratchInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image            = I.scratchImage[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd,
            I.scratchInitialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, b);
        I.scratchInitialized = true;
    }

    // --- Ping-pong schedule (O = beauty, A = scratch0, B = scratch1) so the
    //     final iteration writes back into the beauty image with no copy:
    //       i0: O->A   i1: A->B   i2: B->A   i3: A->B   i4: B->O          ---
    const VkImageView O = inputs.beautyView;
    const VkImageView A = I.scratchView[0];
    const VkImageView B = I.scratchView[1];
    const VkImageView inViews[kNumIterations]  = { O, A, B, A, B };
    const VkImageView outViews[kNumIterations] = { A, B, A, B, O };

    // Update all descriptor sets before recording any dispatch. Each set is
    // consumed by exactly one dispatch, so no in-flight aliasing.
    for (uint32_t it = 0; it < kNumIterations; ++it) {
        VkDescriptorImageInfo ii[4]{};
        ii[0].imageView = inViews[it];       // binding 0: input beauty
        ii[1].imageView = outViews[it];      // binding 1: output beauty
        ii[2].imageView = inputs.normalView; // binding 2: normal AOV
        ii[3].imageView = inputs.depthView;  // binding 3: depth AOV
        for (auto& x : ii) x.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet w[4]{};
        for (uint32_t k = 0; k < 4; ++k) {
            w[k].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet          = I.descriptorSets[it];
            w[k].dstBinding      = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[k].pImageInfo      = &ii[k];
        }
        vkUpdateDescriptorSets(I.device, 4, w, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pipeline);

    const uint32_t gx = (I.width  + 15u) / 16u;
    const uint32_t gy = (I.height + 15u) / 16u;

    for (uint32_t it = 0; it < kNumIterations; ++it) {
        // Between iterations: publish the previous pass's writes to the next
        // pass's reads, and order the WAR on the buffer about to be reused.
        if (it > 0) {
            VkMemoryBarrier mb{};
            mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        AtrousPush pc{};
        pc.stepSize    = 1 << it;   // 1,2,4,8,16
        pc.sigmaColor  = kSigmaColor;
        pc.sigmaNormal = kSigmaNormal;
        pc.sigmaDepth  = kSigmaDepth;
        vkCmdPushConstants(cmd, I.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(AtrousPush), &pc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pipelineLayout, 0, 1,
                                &I.descriptorSets[it], 0, nullptr);
        vkCmdDispatch(cmd, gx, gy, 1);
    }
    // Result now lives in inputs.beautyImage (GENERAL, last written by COMPUTE).
}

}  // namespace ohao
