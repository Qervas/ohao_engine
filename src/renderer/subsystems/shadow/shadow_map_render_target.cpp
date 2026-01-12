#include "subsystems/shadow/shadow_map_render_target.hpp"
#include "vulkan_context.hpp"
#include <iostream>
#include <array>

namespace ohao {

ShadowMapRenderTarget::~ShadowMapRenderTarget() {
    cleanup();
}

bool ShadowMapRenderTarget::initialize(VulkanContext* contextPtr, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) {
        std::cerr << "Invalid dimensions for shadow map: " << w << "x" << h << std::endl;
        return false;
    }

    context = contextPtr;
    width = w;
    height = h;

    if (!context) {
        std::cerr << "Invalid context provided to shadow map render target" << std::endl;
        return false;
    }

    if (!createDepthTarget()) {
        std::cerr << "Failed to create shadow map depth target" << std::endl;
        return false;
    }

    if (!createShadowSampler()) {
        std::cerr << "Failed to create shadow sampler" << std::endl;
        cleanup();
        return false;
    }

    if (!createRenderPass()) {
        std::cerr << "Failed to create shadow render pass" << std::endl;
        cleanup();
        return false;
    }

    if (!createFramebuffer()) {
        std::cerr << "Failed to create shadow framebuffer" << std::endl;
        cleanup();
        return false;
    }

    return true;
}

void ShadowMapRenderTarget::cleanup() {
    if (!context) return;

    auto device = context->getVkDevice();
    context->getLogicalDevice()->waitIdle();

    if (framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }

    if (renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    if (shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, shadowSampler, nullptr);
        shadowSampler = VK_NULL_HANDLE;
    }

    depthTarget.reset();
}

bool ShadowMapRenderTarget::createDepthTarget() {
    depthTarget = std::make_unique<OhaoVkImage>();
    if (!depthTarget->initialize(context->getLogicalDevice())) {
        return false;
    }

    // Create depth-only image for shadow map using D32_SFLOAT for precision
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    if (!depthTarget->createImage(
        width, height,
        depthFormat,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        std::cerr << "Failed to create shadow map depth image" << std::endl;
        return false;
    }

    // Transition depth image to shader read layout
    VkCommandBuffer cmdBuffer = context->getCommandManager()->beginSingleTime();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = depthTarget->getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    context->getCommandManager()->endSingleTime(cmdBuffer);

    if (!depthTarget->createImageView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        std::cerr << "Failed to create shadow map depth image view" << std::endl;
        return false;
    }

    return true;
}

bool ShadowMapRenderTarget::createShadowSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // White = no shadow outside frustum
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE; // We'll do comparison in shader for more control
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;

    if (vkCreateSampler(context->getVkDevice(), &samplerInfo, nullptr, &shadowSampler) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool ShadowMapRenderTarget::createRenderPass() {
    // Depth-only render pass for shadow mapping
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 0;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pColorAttachments = nullptr;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    // Dependencies for proper synchronization
    std::array<VkSubpassDependency, 2> dependencies{};

    // Dependency 0: External -> Subpass
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Dependency 1: Subpass -> External
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(context->getVkDevice(), &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        std::cerr << "Failed to create shadow render pass" << std::endl;
        return false;
    }

    std::cout << "Shadow render pass created successfully" << std::endl;
    return true;
}

bool ShadowMapRenderTarget::createFramebuffer() {
    VkImageView attachment = depthTarget->getImageView();

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &attachment;
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(context->getVkDevice(), &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        std::cerr << "Failed to create shadow framebuffer" << std::endl;
        return false;
    }

    std::cout << "Shadow framebuffer created successfully" << std::endl;
    return true;
}

bool ShadowMapRenderTarget::hasValidRenderTarget() const {
    return depthTarget && shadowSampler && (renderPass != VK_NULL_HANDLE) && (framebuffer != VK_NULL_HANDLE);
}

} // namespace ohao
