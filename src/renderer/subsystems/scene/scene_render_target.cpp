#include "subsystems/scene/scene_render_target.hpp"
#include "vulkan_context.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>


namespace ohao {

SceneRenderTarget::~SceneRenderTarget() {
    cleanup();
}

bool SceneRenderTarget::initialize(VulkanContext* contextPtr, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        std::cerr << "Invalid dimensions for render target: " << width << "x" << height << std::endl;
        return false;
    }

    context = contextPtr;
    if (!context) {
        std::cerr << "Invalid context provided to render target" << std::endl;
        return false;
    }

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
        // Don't free the descriptor set here, let the descriptor pool handle it
        descriptorSet = VK_NULL_HANDLE;
    }

    if (framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }


    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }

    colorTarget.reset();
    depthTarget.reset();
    renderPass.reset();
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
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = colorTarget->getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
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
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Depth attachment
    attachments[1].format = context->getDepthImage()->findDepthFormat(context->getLogicalDevice());
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Attachment references
    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Subpass
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    // Dependencies
    std::array<VkSubpassDependency, 2> dependencies{};

    // Dependency 0: External -> Subpass (Match main render pass)
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Dependency 1: Subpass -> External (Match main render pass)
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = 0;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Create render pass
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    VkRenderPass renderPassHandle;
    if (vkCreateRenderPass(context->getVkDevice(), &renderPassInfo, nullptr, &renderPassHandle) != VK_SUCCESS) {
        return false;
    }

    renderPass = std::make_unique<OhaoVkRenderPass>();
    if (!renderPass->initialize(context->getLogicalDevice(), context->getSwapChain())) {
        return false;
    }
    renderPass->setRenderPass(renderPassHandle);
    return true;
}

bool SceneRenderTarget::createFramebuffer() {
    std::array<VkImageView, 2> attachments = {
        colorTarget->getImageView(),
        depthTarget->getImageView()
    };

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass->getVkRenderPass();
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

    // Free old image descriptor set if any
    if (descriptorSet != VK_NULL_HANDLE) {
        context->getDescriptor()->freeImageDescriptor(descriptorSet);
        descriptorSet = VK_NULL_HANDLE;
    }

    // Prepare descriptor image info
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfo.imageView = colorTarget->getImageView();
    imageInfo.sampler = sampler;

    // Attempt to allocate descriptor set with retry logic
    const int maxAttempts = 3;
    for (int attempts = 0; attempts < maxAttempts; attempts++) {
        descriptorSet = context->getDescriptor()->allocateImageDescriptor(
            colorTarget->getImageView(),
            sampler
        );

        if (descriptorSet != VK_NULL_HANDLE) {
            return true;
        }

        std::cerr << "Attempt " << (attempts + 1) << " to allocate descriptor set failed" << std::endl;
    }

    std::cerr << "Failed to allocate descriptor set after multiple attempts" << std::endl;
    return false;
}

void SceneRenderTarget::resize(uint32_t width, uint32_t height) {
    if (!context) {
        std::cerr << "No valid context for resize operation" << std::endl;
        return;
    }

    if (width == 0 || height == 0) {
        std::cerr << "Invalid dimensions for resize: " << width << "x" << height << std::endl;
        return;
    }

    // Wait for the device to be idle before cleanup
    context->getLogicalDevice()->waitIdle();

    // Store old resources in case we need to rollback
    auto oldColorTarget = std::move(colorTarget);
    auto oldDepthTarget = std::move(depthTarget);
    auto oldSampler = sampler;
    // Keep ownership of the old render pass object so its destructor manages VkRenderPass correctly
    std::unique_ptr<OhaoVkRenderPass> oldRenderPassObj = std::move(renderPass);
    auto oldFramebuffer = framebuffer;
    auto oldDescriptorSet = descriptorSet;

    // Reset handles to prevent accidental use
    sampler = VK_NULL_HANDLE;
    // renderPass is now null after move above
    framebuffer = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;

    bool success = true;

    try {
        if (!createRenderTargets(width, height)) {
            throw std::runtime_error("Failed to create render targets");
        }
        if (!createSampler()) {
            throw std::runtime_error("Failed to create sampler");
        }
        if (!createRenderPass()) {
            throw std::runtime_error("Failed to create render pass");
        }
        if (!createFramebuffer()) {
            throw std::runtime_error("Failed to create framebuffer");
        }
        if (!createDescriptor()) {
            throw std::runtime_error("Failed to create descriptor");
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error during resize: " << e.what() << std::endl;
        success = false;
    }

    if (!success) {
        // Rollback to old resources
        cleanup();  // Clean up any partially created resources

        colorTarget = std::move(oldColorTarget);
        depthTarget = std::move(oldDepthTarget);
        sampler = oldSampler;
        if (!renderPass) {
            renderPass = std::move(oldRenderPassObj);
        }
        framebuffer = oldFramebuffer;
        descriptorSet = oldDescriptorSet;

        std::cerr << "Rolled back to previous state after failed resize" << std::endl;
    }
    else {
        // Clean up old resources
        if (oldSampler != VK_NULL_HANDLE) {
            vkDestroySampler(context->getVkDevice(), oldSampler, nullptr);
        }
        // oldRenderPassObj will be destroyed automatically here
        if (oldFramebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(context->getVkDevice(), oldFramebuffer, nullptr);
        }
        // Free the old image descriptor set now that new one is ready
        if (oldDescriptorSet != VK_NULL_HANDLE) {
            context->getDescriptor()->freeImageDescriptor(oldDescriptorSet);
        }
    }

}

bool SceneRenderTarget::hasValidRenderTarget() const {
    return colorTarget && depthTarget && sampler && renderPass && framebuffer && descriptorSet;
}

} // namespace ohao
