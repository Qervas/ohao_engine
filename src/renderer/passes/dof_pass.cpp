#include "dof_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

DoFPass::~DoFPass() {
    cleanup();
}

bool DoFPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createOutputImages()) return false;
    if (!createDescriptors()) return false;
    if (!createCoCPipeline()) return false;
    if (!createBlurPipeline()) return false;
    if (!createCompositePipeline()) return false;

    std::cout << "DoFPass initialized: " << m_width << "x" << m_height << std::endl;

    return true;
}

void DoFPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_cocPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_cocPipeline, nullptr);
        m_cocPipeline = VK_NULL_HANDLE;
    }
    if (m_blurHPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_blurHPipeline, nullptr);
        m_blurHPipeline = VK_NULL_HANDLE;
    }
    if (m_blurVPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_blurVPipeline, nullptr);
        m_blurVPipeline = VK_NULL_HANDLE;
    }
    if (m_compositePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_compositePipeline, nullptr);
        m_compositePipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }

    destroyResources();
}

void DoFPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_colorView == VK_NULL_HANDLE ||
        m_depthView == VK_NULL_HANDLE ||
        m_cocPipeline == VK_NULL_HANDLE) {
        return;
    }

    DoFParams params{};
    params.screenSize = glm::vec4(m_width, m_height, 1.0f / m_width, 1.0f / m_height);
    params.focusParams = glm::vec4(m_focalLength, m_aperture, m_focusDistance, m_sensorSize);
    params.blurRegions = glm::vec4(m_nearStart, m_nearEnd, m_farStart, m_farEnd);
    params.maxBlurRadius = m_maxBlurRadius;
    params.nearPlane = m_nearPlane;
    params.farPlane = m_farPlane;
    params.bokehBlades = m_bokehBlades;

    // Pass 1: Calculate Circle of Confusion
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cocPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(DoFParams), &params);

    uint32_t groupsX = (m_width + 7) / 8;
    uint32_t groupsY = (m_height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Barrier: CoC calculation complete
    VkImageMemoryBarrier cocBarrier{};
    cocBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    cocBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cocBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    cocBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    cocBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    cocBarrier.image = m_cocImage;
    cocBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cocBarrier.subresourceRange.baseMipLevel = 0;
    cocBarrier.subresourceRange.levelCount = 1;
    cocBarrier.subresourceRange.baseArrayLayer = 0;
    cocBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &cocBarrier);

    // Pass 2: Horizontal blur (for now, use a single combined blur pass)
    // A full implementation would do separable blur in two passes

    // Pass 3: Composite final result
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compositePipeline);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Barrier for output
    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    outputBarrier.image = m_outputImage;
    outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.baseMipLevel = 0;
    outputBarrier.subresourceRange.levelCount = 1;
    outputBarrier.subresourceRange.baseArrayLayer = 0;
    outputBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &outputBarrier);
}

void DoFPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyResources();
    createOutputImages();
    updateDescriptorSet();
}

void DoFPass::setColorBuffer(VkImageView color) {
    m_colorView = color;
}

void DoFPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
}

void DoFPass::updateDescriptorSet() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    std::array<VkWriteDescriptorSet, 4> writes{};
    std::array<VkDescriptorImageInfo, 4> imageInfos{};

    // Color buffer (input)
    imageInfos[0].sampler = m_sampler;
    imageInfos[0].imageView = m_colorView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfos[0];

    // Depth buffer (input)
    imageInfos[1].sampler = m_sampler;
    imageInfos[1].imageView = m_depthView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfos[1];

    // CoC buffer (storage)
    imageInfos[2].sampler = VK_NULL_HANDLE;
    imageInfos[2].imageView = m_cocView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imageInfos[2];

    // Output (storage)
    imageInfos[3].sampler = VK_NULL_HANDLE;
    imageInfos[3].imageView = m_outputView;
    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &imageInfos[3];

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool DoFPass::createOutputImages() {
    // Create sampler first
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        return false;
    }

    // Helper lambda to create image + memory + view
    auto createImage = [this](VkFormat format, VkImage& image, VkDeviceMemory& memory,
                               VkImageView& view, uint32_t w, uint32_t h) -> bool {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {w, h, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, image, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }

        vkBindImageMemory(m_device, image, memory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        return vkCreateImageView(m_device, &viewInfo, nullptr, &view) == VK_SUCCESS;
    };

    // CoC buffer (single channel, half res)
    if (!createImage(VK_FORMAT_R16_SFLOAT, m_cocImage, m_cocMemory, m_cocView,
                     m_width, m_height)) {
        return false;
    }

    // Output image
    if (!createImage(VK_FORMAT_R16G16B16A16_SFLOAT, m_outputImage, m_outputMemory,
                     m_outputView, m_width, m_height)) {
        return false;
    }

    return true;
}

bool DoFPass::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

    // Color buffer (sampled)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Depth buffer (sampled)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // CoC buffer (storage)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Output (storage)
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool DoFPass::createCoCPipeline() {
    VkShaderModule compShader = loadShaderModule("compute_dof_coc.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "Failed to load DoF CoC shader" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = compShader;
    shaderStage.pName = "main";

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(DoFParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &m_cocPipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create DoF CoC pipeline" << std::endl;
        return false;
    }

    std::cout << "DoF CoC pipeline created successfully" << std::endl;
    return true;
}

bool DoFPass::createBlurPipeline() {
    // For now, we'll do a single-pass blur in the composite shader
    // A full implementation would have horizontal and vertical blur passes
    return true;
}

bool DoFPass::createCompositePipeline() {
    VkShaderModule compShader = loadShaderModule("compute_dof_composite.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "Failed to load DoF composite shader" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = compShader;
    shaderStage.pName = "main";

    // Reuse existing pipeline layout
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &m_compositePipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create DoF composite pipeline" << std::endl;
        return false;
    }

    std::cout << "DoF composite pipeline created successfully" << std::endl;
    return true;
}

void DoFPass::destroyResources() {
    auto destroyImage = [this](VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    };

    destroyImage(m_cocImage, m_cocMemory, m_cocView);
    destroyImage(m_nearImage, m_nearMemory, m_nearView);
    destroyImage(m_farImage, m_farMemory, m_farView);
    destroyImage(m_tempImage, m_tempMemory, m_tempView);
    destroyImage(m_outputImage, m_outputMemory, m_outputView);
}

} // namespace ohao
