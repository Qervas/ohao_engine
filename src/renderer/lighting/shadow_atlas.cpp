#include "shadow_atlas.hpp"
#include "renderer/vulkan_context.hpp"
#include "rhi/vk/ohao_vk_image.hpp"
#include "rhi/vk/ohao_vk_command_manager.hpp"
#include <iostream>
#include <array>

namespace ohao {

ShadowAtlas::~ShadowAtlas() {
    cleanup();
}

bool ShadowAtlas::initialize(VulkanContext* ctx) {
    if (!ctx) {
        std::cerr << "[ShadowAtlas] Invalid VulkanContext" << std::endl;
        return false;
    }

    context = ctx;

    if (!createAtlasImage()) {
        std::cerr << "[ShadowAtlas] Failed to create atlas image" << std::endl;
        cleanup();
        return false;
    }

    if (!createShadowSampler()) {
        std::cerr << "[ShadowAtlas] Failed to create shadow sampler" << std::endl;
        cleanup();
        return false;
    }

    if (!createRenderPass()) {
        std::cerr << "[ShadowAtlas] Failed to create render pass" << std::endl;
        cleanup();
        return false;
    }

    if (!createFramebuffer()) {
        std::cerr << "[ShadowAtlas] Failed to create framebuffer" << std::endl;
        cleanup();
        return false;
    }

    // Clear allocation tracking
    allocatedTiles.reset();

    initialized = true;
    std::cout << "[ShadowAtlas] Initialized " << kAtlasSize << "x" << kAtlasSize
              << " atlas with " << kTotalTiles << " tiles (" << kTileSize << "x" << kTileSize << " each)"
              << std::endl;
    return true;
}

void ShadowAtlas::cleanup() {
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

    atlasImage.reset();
    allocatedTiles.reset();
    initialized = false;
    context = nullptr;
}

bool ShadowAtlas::createAtlasImage() {
    atlasImage = std::make_unique<OhaoVkImage>();
    if (!atlasImage->initialize(context->getLogicalDevice())) {
        return false;
    }

    // Create single large depth image for atlas
    if (!atlasImage->createImage(
            kAtlasSize, kAtlasSize,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        std::cerr << "[ShadowAtlas] Failed to create atlas depth image" << std::endl;
        return false;
    }

    // Transition to depth attachment layout
    VkCommandBuffer cmdBuffer = context->getCommandManager()->beginSingleTime();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = atlasImage->getImage();
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

    // Create image view
    if (!atlasImage->createImageView(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        std::cerr << "[ShadowAtlas] Failed to create atlas image view" << std::endl;
        return false;
    }

    return true;
}

bool ShadowAtlas::createRenderPass() {
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

    // Subpass dependencies
    std::array<VkSubpassDependency, 2> dependencies{};

    // External -> Subpass
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Subpass -> External
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
        return false;
    }

    return true;
}

bool ShadowAtlas::createFramebuffer() {
    VkImageView attachment = atlasImage->getImageView();

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &attachment;
    framebufferInfo.width = kAtlasSize;
    framebufferInfo.height = kAtlasSize;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(context->getVkDevice(), &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool ShadowAtlas::createShadowSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
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

std::optional<AtlasAllocation> ShadowAtlas::allocateTile() {
    if (!initialized) {
        return std::nullopt;
    }

    // Find first free tile
    for (uint32_t i = 0; i < kTotalTiles; ++i) {
        if (!allocatedTiles.test(i)) {
            allocatedTiles.set(i);

            AtlasAllocation alloc;
            alloc.handle = AtlasTileHandle{i};

            // Calculate UV offset and scale
            auto [row, col] = tileIndexToRowCol(i);
            alloc.uvOffset = glm::vec2(
                static_cast<float>(col) * kTileUVScale,
                static_cast<float>(row) * kTileUVScale
            );
            alloc.uvScale = glm::vec2(kTileUVScale);

            // Calculate viewport
            auto [pixelX, pixelY] = rowColToPixelOffset(row, col);
            alloc.viewport.x = static_cast<float>(pixelX);
            alloc.viewport.y = static_cast<float>(pixelY);
            alloc.viewport.width = static_cast<float>(kTileSize);
            alloc.viewport.height = static_cast<float>(kTileSize);
            alloc.viewport.minDepth = 0.0f;
            alloc.viewport.maxDepth = 1.0f;

            // Calculate scissor
            alloc.scissorRect.offset = {static_cast<int32_t>(pixelX), static_cast<int32_t>(pixelY)};
            alloc.scissorRect.extent = {kTileSize, kTileSize};

            return alloc;
        }
    }

    // Atlas is full - return nullopt (caller must handle gracefully)
    return std::nullopt;
}

void ShadowAtlas::releaseTile(AtlasTileHandle handle) {
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return;
    }
    allocatedTiles.reset(handle.id);
}

bool ShadowAtlas::isTileAllocated(AtlasTileHandle handle) const {
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return false;
    }
    return allocatedTiles.test(handle.id);
}

uint32_t ShadowAtlas::getAllocatedTileCount() const {
    return static_cast<uint32_t>(allocatedTiles.count());
}

glm::vec2 ShadowAtlas::getTileUVOffset(AtlasTileHandle handle) const {
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return glm::vec2(0.0f);
    }

    auto [row, col] = tileIndexToRowCol(handle.id);
    return glm::vec2(
        static_cast<float>(col) * kTileUVScale,
        static_cast<float>(row) * kTileUVScale
    );
}

VkViewport ShadowAtlas::getTileViewport(AtlasTileHandle handle) const {
    VkViewport viewport{};
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return viewport;
    }

    auto [row, col] = tileIndexToRowCol(handle.id);
    auto [pixelX, pixelY] = rowColToPixelOffset(row, col);

    viewport.x = static_cast<float>(pixelX);
    viewport.y = static_cast<float>(pixelY);
    viewport.width = static_cast<float>(kTileSize);
    viewport.height = static_cast<float>(kTileSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    return viewport;
}

VkRect2D ShadowAtlas::getTileScissor(AtlasTileHandle handle) const {
    VkRect2D scissor{};
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return scissor;
    }

    auto [row, col] = tileIndexToRowCol(handle.id);
    auto [pixelX, pixelY] = rowColToPixelOffset(row, col);

    scissor.offset = {static_cast<int32_t>(pixelX), static_cast<int32_t>(pixelY)};
    scissor.extent = {kTileSize, kTileSize};

    return scissor;
}

AtlasTileInfo ShadowAtlas::getTileInfo(AtlasTileHandle handle, const glm::mat4& lightSpaceMatrix) const {
    AtlasTileInfo info{};

    if (!handle.isValid() || handle.id >= kTotalTiles) {
        info.uvOffset = glm::vec2(0.0f);
        info.uvScale = glm::vec2(0.0f);
        info.viewProj = glm::mat4(1.0f);
        return info;
    }

    info.uvOffset = getTileUVOffset(handle);
    info.uvScale = glm::vec2(kTileUVScale);
    info.viewProj = lightSpaceMatrix;

    return info;
}

void ShadowAtlas::beginRenderPass(VkCommandBuffer cmd) {
    if (!initialized) return;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {kAtlasSize, kAtlasSize};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void ShadowAtlas::endRenderPass(VkCommandBuffer cmd) {
    if (!initialized) return;
    vkCmdEndRenderPass(cmd);
}

void ShadowAtlas::setTileViewportScissor(VkCommandBuffer cmd, AtlasTileHandle handle) {
    if (!handle.isValid() || handle.id >= kTotalTiles) {
        return;
    }

    VkViewport viewport = getTileViewport(handle);
    VkRect2D scissor = getTileScissor(handle);

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkImageView ShadowAtlas::getImageView() const {
    return atlasImage ? atlasImage->getImageView() : VK_NULL_HANDLE;
}

} // namespace ohao
