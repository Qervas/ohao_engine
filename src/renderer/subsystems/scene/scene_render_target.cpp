#include "subsystems/scene/scene_render_target.hpp"
#include "vulkan_context.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>


namespace ohao {

SceneRenderTarget::~SceneRenderTarget() {
    cleanup();
}

bool SceneRenderTarget::initialize(VulkanContext* contextPtr, uint32_t width, uint32_t height) {
    context = contextPtr;

    // Create resources in the correct order
    if (!createRenderTargets(width, height)) {
        std::cerr << "Failed to create render targets" << std::endl;
        return false;
    }

    if (!createSampler()) {
        std::cerr << "Failed to create sampler" << std::endl;
        cleanup();
        return false;
    }

    if (!createRenderPass()) {
        std::cerr << "Failed to create render pass" << std::endl;
        cleanup();
        return false;
    }

    if (!createFramebuffer()) {
        std::cerr << "Failed to create framebuffer" << std::endl;
        cleanup();
        return false;
    }

    // Create descriptor last, after all resources are ready
    if (!createDescriptor()) {
        std::cerr << "Failed to create descriptor" << std::endl;
        cleanup();
        return false;
    }

    return true;
}


void SceneRenderTarget::cleanup() {
    if (!context) return;

    auto device = context->getVkDevice();
    context->getLogicalDevice()->waitIdle();


    if (descriptorSet != VK_NULL_HANDLE) {
        // Note: Descriptor sets are freed when their pool is destroyed
        descriptorSet = VK_NULL_HANDLE;
    }

    if (framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }

    if (renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }

    colorTarget.reset();
    depthTarget.reset();
}

bool SceneRenderTarget::createRenderTargets(uint32_t width, uint32_t height) {
    // Create color target
    colorTarget = std::make_unique<OhaoVkImage>();
    if (!colorTarget->initialize(context->getLogicalDevice())) {
        return false;
    }

    if (!colorTarget->createImage(
        width, height,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        return false;
    }

    // Transition color image to shader read layout
    VkCommandBuffer cmdBuffer = context->getCommandManager()->beginSingleTime();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = colorTarget->getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    context->getCommandManager()->endSingleTime(cmdBuffer);

    if (!colorTarget->createImageView(
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }


    // Create depth target
    depthTarget = std::make_unique<OhaoVkImage>();
    if (!depthTarget->initialize(context->getLogicalDevice())) {
        return false;
    }

    if (!depthTarget->createDepthResources(
        {width, height},
        VK_SAMPLE_COUNT_1_BIT)) {
        return false;
    }

    return true;
}

bool SceneRenderTarget::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(context->getVkDevice(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool SceneRenderTarget::createRenderPass() {
    std::array<VkAttachmentDescription, 2> attachments{};

    // Color attachment
    attachments[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth attachment
    attachments[1].format = context->getDepthImage()->findDepthFormat(context->getLogicalDevice());
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};

    // First dependency - External -> This pass
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_NONE;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Second dependency - This pass -> External
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_NONE;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(context->getVkDevice(), &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool SceneRenderTarget::createFramebuffer() {
    std::array<VkImageView, 2> attachments = {
        colorTarget->getImageView(),
        depthTarget->getImageView()
    };

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = colorTarget->getWidth();
    framebufferInfo.height = colorTarget->getHeight();
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(context->getVkDevice(), &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool SceneRenderTarget::createDescriptor() {
    if (!colorTarget || !colorTarget->getImageView() || !sampler) {
        std::cerr << "Required resources not available for descriptor creation" << std::endl;
        return false;
    }

    descriptorSet = context->getDescriptor()->allocateImageDescriptor(
        colorTarget->getImageView(),
        sampler
    );

    if (descriptorSet == VK_NULL_HANDLE) {
        std::cerr << "Failed to allocate image descriptor set" << std::endl;
        return false;
    }

    return true;
}

void SceneRenderTarget::resize(uint32_t width, uint32_t height) {
    if (context) {
        context->getLogicalDevice()->waitIdle();
    }
    cleanup();
    initialize(context, width, height);
}

} // namespace ohao