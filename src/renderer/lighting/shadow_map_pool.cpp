#include "shadow_map_pool.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/rhi/vk/ohao_vk_device.hpp"
#include "renderer/rhi/vk/ohao_vk_physical_device.hpp"
#include <iostream>
#include <array>

namespace ohao {

ShadowMapPool::~ShadowMapPool() {
    cleanup();
}

bool ShadowMapPool::initialize(VulkanContext* ctx) {
    context = ctx;
    device = ctx->getLogicalDevice();

    if (!createShadowRenderPass()) {
        std::cerr << "ShadowMapPool: Failed to create render pass" << std::endl;
        return false;
    }

    if (!createShadowMaps()) {
        std::cerr << "ShadowMapPool: Failed to create shadow maps" << std::endl;
        cleanup();
        return false;
    }

    if (!createFramebuffers()) {
        std::cerr << "ShadowMapPool: Failed to create framebuffers" << std::endl;
        cleanup();
        return false;
    }

    if (!createShadowSampler()) {
        std::cerr << "ShadowMapPool: Failed to create sampler" << std::endl;
        cleanup();
        return false;
    }

    if (!createPlaceholderTexture()) {
        std::cerr << "ShadowMapPool: Failed to create placeholder texture" << std::endl;
        cleanup();
        return false;
    }

    // Transition all images to SHADER_READ_ONLY_OPTIMAL layout so they can be bound in descriptors
    transitionImagesToShaderReadLayout();

    initialized = true;
    std::cout << "ShadowMapPool: Initialized with " << MAX_SHADOW_MAPS
              << " shadow maps (" << SHADOW_MAP_SIZE << "x" << SHADOW_MAP_SIZE << ")" << std::endl;
    return true;
}

void ShadowMapPool::cleanup() {
    if (!device) return;

    VkDevice vkDevice = device->getDevice();
    device->waitIdle();

    // Cleanup shadow maps
    for (auto& sm : shadowMaps) {
        if (sm.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(vkDevice, sm.imageView, nullptr);
            sm.imageView = VK_NULL_HANDLE;
        }
        if (sm.image != VK_NULL_HANDLE) {
            vkDestroyImage(vkDevice, sm.image, nullptr);
            sm.image = VK_NULL_HANDLE;
        }
        if (sm.memory != VK_NULL_HANDLE) {
            vkFreeMemory(vkDevice, sm.memory, nullptr);
            sm.memory = VK_NULL_HANDLE;
        }
        sm.inUse = false;
    }

    // Cleanup framebuffers
    for (auto& fb : framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(vkDevice, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
    }

    // Cleanup placeholder
    if (placeholderImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(vkDevice, placeholderImageView, nullptr);
        placeholderImageView = VK_NULL_HANDLE;
    }
    if (placeholderImage != VK_NULL_HANDLE) {
        vkDestroyImage(vkDevice, placeholderImage, nullptr);
        placeholderImage = VK_NULL_HANDLE;
    }
    if (placeholderMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vkDevice, placeholderMemory, nullptr);
        placeholderMemory = VK_NULL_HANDLE;
    }

    // Cleanup sampler
    if (shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(vkDevice, shadowSampler, nullptr);
        shadowSampler = VK_NULL_HANDLE;
    }

    // Cleanup render pass
    if (shadowRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vkDevice, shadowRenderPass, nullptr);
        shadowRenderPass = VK_NULL_HANDLE;
    }

    initialized = false;
}

bool ShadowMapPool::createShadowRenderPass() {
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pColorAttachments = nullptr;
    subpass.pDepthStencilAttachment = &depthRef;

    // Subpass dependencies for proper synchronization
    std::array<VkSubpassDependency, 2> dependencies{};

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

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

    if (vkCreateRenderPass(device->getDevice(), &renderPassInfo, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool ShadowMapPool::createShadowMaps() {
    VkDevice vkDevice = device->getDevice();

    for (uint32_t i = 0; i < MAX_SHADOW_MAPS; ++i) {
        // Create depth image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(vkDevice, &imageInfo, nullptr, &shadowMaps[i].image) != VK_SUCCESS) {
            std::cerr << "ShadowMapPool: Failed to create shadow map image " << i << std::endl;
            return false;
        }

        // Allocate memory
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(vkDevice, shadowMaps[i].image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device->getPhysicalDevice()->findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );

        if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &shadowMaps[i].memory) != VK_SUCCESS) {
            std::cerr << "ShadowMapPool: Failed to allocate shadow map memory " << i << std::endl;
            return false;
        }

        vkBindImageMemory(vkDevice, shadowMaps[i].image, shadowMaps[i].memory, 0);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = shadowMaps[i].image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &shadowMaps[i].imageView) != VK_SUCCESS) {
            std::cerr << "ShadowMapPool: Failed to create shadow map image view " << i << std::endl;
            return false;
        }

        shadowMaps[i].inUse = false;
    }

    return true;
}

bool ShadowMapPool::createFramebuffers() {
    VkDevice vkDevice = device->getDevice();

    for (uint32_t i = 0; i < MAX_SHADOW_MAPS; ++i) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = shadowRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &shadowMaps[i].imageView;
        framebufferInfo.width = SHADOW_MAP_SIZE;
        framebufferInfo.height = SHADOW_MAP_SIZE;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            std::cerr << "ShadowMapPool: Failed to create framebuffer " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool ShadowMapPool::createShadowSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // White = no shadow outside
    samplerInfo.compareEnable = VK_FALSE;  // We do comparison in shader
    samplerInfo.compareOp = VK_COMPARE_OP_LESS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;

    if (vkCreateSampler(device->getDevice(), &samplerInfo, nullptr, &shadowSampler) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool ShadowMapPool::createPlaceholderTexture() {
    VkDevice vkDevice = device->getDevice();

    // Create 1x1 white depth texture for unused shadow map slots
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {1, 1, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &placeholderImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(vkDevice, placeholderImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device->getPhysicalDevice()->findMemoryType(
        memRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &placeholderMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(vkDevice, placeholderImage, placeholderMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = placeholderImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &placeholderImageView) != VK_SUCCESS) {
        return false;
    }

    return true;
}

ShadowMapHandle ShadowMapPool::allocate() {
    for (uint32_t i = 0; i < MAX_SHADOW_MAPS; ++i) {
        if (!shadowMaps[i].inUse) {
            shadowMaps[i].inUse = true;
            return ShadowMapHandle{i};
        }
    }
    return ShadowMapHandle::invalid();
}

void ShadowMapPool::release(ShadowMapHandle handle) {
    if (handle.isValid() && handle.id < MAX_SHADOW_MAPS) {
        shadowMaps[handle.id].inUse = false;
    }
}

VkImageView ShadowMapPool::getImageView(ShadowMapHandle handle) const {
    if (handle.isValid() && handle.id < MAX_SHADOW_MAPS) {
        return shadowMaps[handle.id].imageView;
    }
    return placeholderImageView;
}

VkImage ShadowMapPool::getImage(ShadowMapHandle handle) const {
    if (handle.isValid() && handle.id < MAX_SHADOW_MAPS) {
        return shadowMaps[handle.id].image;
    }
    return placeholderImage;
}

std::array<VkImageView, MAX_SHADOW_MAPS> ShadowMapPool::getAllImageViews() const {
    std::array<VkImageView, MAX_SHADOW_MAPS> views;
    for (uint32_t i = 0; i < MAX_SHADOW_MAPS; ++i) {
        if (shadowMaps[i].imageView != VK_NULL_HANDLE) {
            views[i] = shadowMaps[i].imageView;
        } else {
            views[i] = placeholderImageView;
        }
    }
    return views;
}

VkFramebuffer ShadowMapPool::getFramebuffer(ShadowMapHandle handle) const {
    if (handle.isValid() && handle.id < MAX_SHADOW_MAPS) {
        return framebuffers[handle.id];
    }
    return VK_NULL_HANDLE;
}

void ShadowMapPool::beginShadowPass(VkCommandBuffer cmd, ShadowMapHandle handle) {
    if (!handle.isValid() || handle.id >= MAX_SHADOW_MAPS) return;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = shadowRenderPass;
    renderPassInfo.framebuffer = framebuffers[handle.id];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.height = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void ShadowMapPool::endShadowPass(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

void ShadowMapPool::transitionImagesToShaderReadLayout() {
    if (!context) return;

    VkDevice vkDevice = device->getDevice();
    VkCommandPool cmdPool = context->getVkCommandPool();
    VkQueue graphicsQueue = device->getGraphicsQueue();

    // Create a one-time command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = cmdPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmdBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    // Transition all shadow maps to SHADER_READ_ONLY_OPTIMAL
    for (uint32_t i = 0; i < MAX_SHADOW_MAPS; ++i) {
        if (shadowMaps[i].image == VK_NULL_HANDLE) continue;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadowMaps[i].image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
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
    }

    // Also transition placeholder image
    if (placeholderImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = placeholderImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
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
    }

    vkEndCommandBuffer(cmdBuffer);

    // Submit and wait
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(vkDevice, cmdPool, 1, &cmdBuffer);

    std::cout << "ShadowMapPool: Transitioned all shadow maps to SHADER_READ_ONLY_OPTIMAL layout" << std::endl;
}

} // namespace ohao
