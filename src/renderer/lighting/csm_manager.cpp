#include "csm_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "rhi/vk/ohao_vk_image.hpp"
#include "rhi/vk/ohao_vk_uniform_buffer.hpp"
#include "rhi/vk/ohao_vk_command_manager.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace ohao {

CSMManager::~CSMManager() {
    cleanup();
}

bool CSMManager::initialize(VulkanContext* ctx, const CSMConfig& cfg) {
    if (!ctx) {
        std::cerr << "[CSM] Invalid VulkanContext" << std::endl;
        return false;
    }

    context = ctx;
    config = cfg;

    // Create resources in order
    if (!createDepthImages()) {
        std::cerr << "[CSM] Failed to create depth images" << std::endl;
        cleanup();
        return false;
    }

    if (!createShadowSampler()) {
        std::cerr << "[CSM] Failed to create shadow sampler" << std::endl;
        cleanup();
        return false;
    }

    if (!createRenderPass()) {
        std::cerr << "[CSM] Failed to create render pass" << std::endl;
        cleanup();
        return false;
    }

    if (!createFramebuffers()) {
        std::cerr << "[CSM] Failed to create framebuffers" << std::endl;
        cleanup();
        return false;
    }

    if (!createUBO()) {
        std::cerr << "[CSM] Failed to create UBO" << std::endl;
        cleanup();
        return false;
    }

    initialized = true;
    std::cout << "[CSM] Initialized with " << kNumCascades << " cascades at "
              << config.cascadeResolution << "x" << config.cascadeResolution << std::endl;
    return true;
}

void CSMManager::cleanup() {
    if (!context) return;

    auto device = context->getVkDevice();
    context->getLogicalDevice()->waitIdle();

    // Destroy framebuffers
    for (auto& fb : cascadeFramebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
    }

    // Destroy render pass
    if (renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    // Destroy sampler
    if (shadowSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, shadowSampler, nullptr);
        shadowSampler = VK_NULL_HANDLE;
    }

    // Destroy depth images
    for (auto& img : cascadeDepthImages) {
        img.reset();
    }

    // Destroy UBO
    csmUBO.reset();

    initialized = false;
    context = nullptr;
}

bool CSMManager::createDepthImages() {
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        cascadeDepthImages[i] = std::make_unique<OhaoVkImage>();
        if (!cascadeDepthImages[i]->initialize(context->getLogicalDevice())) {
            std::cerr << "[CSM] Failed to initialize depth image " << i << std::endl;
            return false;
        }

        // Create depth image
        if (!cascadeDepthImages[i]->createImage(
                config.cascadeResolution,
                config.cascadeResolution,
                VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            std::cerr << "[CSM] Failed to create depth image " << i << std::endl;
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
        barrier.image = cascadeDepthImages[i]->getImage();
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
        if (!cascadeDepthImages[i]->createImageView(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
            std::cerr << "[CSM] Failed to create depth image view " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool CSMManager::createRenderPass() {
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

bool CSMManager::createFramebuffers() {
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        VkImageView attachment = cascadeDepthImages[i]->getImageView();

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = config.cascadeResolution;
        framebufferInfo.height = config.cascadeResolution;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(context->getVkDevice(), &framebufferInfo, nullptr, &cascadeFramebuffers[i]) != VK_SUCCESS) {
            std::cerr << "[CSM] Failed to create framebuffer " << i << std::endl;
            return false;
        }
    }

    return true;
}

bool CSMManager::createShadowSampler() {
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

bool CSMManager::createUBO() {
    csmUBO = std::make_unique<OhaoVkUniformBuffer>();
    // 2 frames for double buffering
    if (!csmUBO->initialize(context->getLogicalDevice(), 2, sizeof(CSMUBO))) {
        return false;
    }
    return true;
}

void CSMManager::calculateSplitDepths(float nearClip, float farClip,
                                       std::array<float, kNumCascades + 1>& splits) const {
    const float lambda = config.splitLambda;
    const float ratio = farClip / nearClip;

    splits[0] = nearClip;

    for (uint32_t i = 1; i < kNumCascades; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(kNumCascades);

        // Logarithmic split
        const float logSplit = nearClip * std::pow(ratio, p);

        // Uniform split
        const float uniformSplit = nearClip + (farClip - nearClip) * p;

        // Blend between logarithmic and uniform
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }

    splits[kNumCascades] = farClip;
}

void CSMManager::getFrustumCornersWorldSpace(
    const glm::mat4& invViewProj,
    float splitNear,
    float splitFar,
    glm::vec4 outCorners[8]) const
{
    // NDC corners (Vulkan: Z in [0, 1])
    const glm::vec3 ndcCorners[8] = {
        // Near plane
        {-1.0f, -1.0f, splitNear},
        { 1.0f, -1.0f, splitNear},
        { 1.0f,  1.0f, splitNear},
        {-1.0f,  1.0f, splitNear},
        // Far plane
        {-1.0f, -1.0f, splitFar},
        { 1.0f, -1.0f, splitFar},
        { 1.0f,  1.0f, splitFar},
        {-1.0f,  1.0f, splitFar}
    };

    for (int i = 0; i < 8; ++i) {
        glm::vec4 worldPos = invViewProj * glm::vec4(ndcCorners[i], 1.0f);
        outCorners[i] = worldPos / worldPos.w;
    }
}

glm::mat4 CSMManager::calculateCascadeMatrix(
    uint32_t cascadeIdx,
    const glm::vec3& lightDir,
    const glm::vec4 frustumCorners[8],
    bool stabilize) const
{
    // Calculate frustum center
    glm::vec3 frustumCenter(0.0f);
    for (int i = 0; i < 8; ++i) {
        frustumCenter += glm::vec3(frustumCorners[i]);
    }
    frustumCenter /= 8.0f;

    // Calculate bounding sphere radius for stabilization
    float radius = 0.0f;
    for (int i = 0; i < 8; ++i) {
        float distance = glm::length(glm::vec3(frustumCorners[i]) - frustumCenter);
        radius = std::max(radius, distance);
    }

    // Light view matrix
    glm::vec3 lightPos = frustumCenter - glm::normalize(lightDir) * radius * 2.0f;
    glm::vec3 up = glm::abs(lightDir.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::mat4 lightView = glm::lookAt(lightPos, frustumCenter, up);

    // Calculate AABB in light space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (int i = 0; i < 8; ++i) {
        glm::vec4 lightSpaceCorner = lightView * frustumCorners[i];
        minX = std::min(minX, lightSpaceCorner.x);
        maxX = std::max(maxX, lightSpaceCorner.x);
        minY = std::min(minY, lightSpaceCorner.y);
        maxY = std::max(maxY, lightSpaceCorner.y);
        minZ = std::min(minZ, lightSpaceCorner.z);
        maxZ = std::max(maxZ, lightSpaceCorner.z);
    }

    // Stabilization: snap to texel boundaries
    if (stabilize) {
        float worldUnitsPerTexel = (maxX - minX) / static_cast<float>(config.cascadeResolution);

        minX = std::floor(minX / worldUnitsPerTexel) * worldUnitsPerTexel;
        maxX = std::floor(maxX / worldUnitsPerTexel) * worldUnitsPerTexel;
        minY = std::floor(minY / worldUnitsPerTexel) * worldUnitsPerTexel;
        maxY = std::floor(maxY / worldUnitsPerTexel) * worldUnitsPerTexel;
    }

    // Extend Z range for shadow casters behind camera
    const float zMargin = (maxZ - minZ) * 10.0f;
    minZ -= zMargin;

    // Create orthographic projection
    glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, -maxZ, -minZ);

    // Convert from OpenGL Z range [-1, 1] to Vulkan Z range [0, 1]
    lightProj[2][2] = -1.0f / (-minZ - (-maxZ));
    lightProj[3][2] = -(-maxZ) / (-minZ - (-maxZ));

    // Flip Y for Vulkan
    lightProj[1][1] *= -1.0f;

    return lightProj * lightView;
}

void CSMManager::update(
    const glm::mat4& cameraView,
    const glm::mat4& cameraProj,
    const glm::vec3& lightDir,
    float cameraNear,
    float cameraFar)
{
    if (!initialized) return;

    // Use shadow distance if smaller than camera far
    float shadowFar = std::min(cameraFar, config.shadowDistance);

    // Calculate split depths
    std::array<float, kNumCascades + 1> splitDepths;
    calculateSplitDepths(cameraNear, shadowFar, splitDepths);

    // Inverse view-projection for frustum corners
    glm::mat4 invViewProj = glm::inverse(cameraProj * cameraView);

    // Calculate matrices for each cascade
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        // Convert view-space depths to NDC depths (Vulkan: 0 to 1)
        // For perspective projection: ndc_z = (f - z) / (f - n) in Vulkan
        // This is approximate; for accuracy we use the frustum corners approach
        float splitNearNDC = 0.0f;
        float splitFarNDC = 1.0f;

        if (i > 0) {
            // Calculate NDC depth from view-space depth
            // Using perspective projection formula for Vulkan
            float z = splitDepths[i];
            splitNearNDC = (cameraFar - z) / (cameraFar - cameraNear);
            splitNearNDC = glm::clamp(splitNearNDC, 0.0f, 1.0f);
        }

        {
            float z = splitDepths[i + 1];
            splitFarNDC = (cameraFar - z) / (cameraFar - cameraNear);
            splitFarNDC = glm::clamp(splitFarNDC, 0.0f, 1.0f);
        }

        // Get frustum corners in world space
        glm::vec4 frustumCorners[8];
        getFrustumCornersWorldSpace(invViewProj, splitNearNDC, splitFarNDC, frustumCorners);

        // Calculate light-space matrix
        cascadeData[i].viewProj = calculateCascadeMatrix(i, lightDir, frustumCorners, config.stabilize);
        cascadeData[i].splitNear = splitDepths[i];
        cascadeData[i].splitFar = splitDepths[i + 1];
        cascadeData[i].texelSize = (cascadeData[i].splitFar - cascadeData[i].splitNear) /
                                    static_cast<float>(config.cascadeResolution);

        // Store frustum corners for debug visualization
        for (int j = 0; j < 8; ++j) {
            cascadeData[i].frustumCorners[j] = frustumCorners[j];
        }
    }
}

void CSMManager::beginCascade(VkCommandBuffer cmd, CascadeIndex cascadeIndex) {
    if (!cascadeIndex.isValid() || cascadeIndex.id >= kNumCascades) {
        std::cerr << "[CSM] Invalid cascade index: " << cascadeIndex.id << std::endl;
        return;
    }

    currentCascade = static_cast<int32_t>(cascadeIndex.id);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = cascadeFramebuffers[currentCascade];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {config.cascadeResolution, config.cascadeResolution};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(config.cascadeResolution);
    viewport.height = static_cast<float>(config.cascadeResolution);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {config.cascadeResolution, config.cascadeResolution};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void CSMManager::endCascade(VkCommandBuffer cmd) {
    if (currentCascade < 0) return;

    vkCmdEndRenderPass(cmd);
    currentCascade = -1;
}

void CSMManager::updateUBO(uint32_t frameIndex) {
    if (!csmUBO || !initialized) return;

    CSMUBO uboData{};

    // Fill cascade info
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        uboData.cascades[i].viewProj = cascadeData[i].viewProj;
        uboData.cascades[i].splitDepth = cascadeData[i].splitFar;
        uboData.cascades[i].texelSize = cascadeData[i].texelSize;
    }

    // Fill split depths array
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        uboData.cascadeSplitDepths[i] = cascadeData[i].splitFar;
    }

    uboData.numCascades = static_cast<int32_t>(kNumCascades);
    uboData.shadowBias = config.shadowBias;
    uboData.normalBias = config.normalBias;

    csmUBO->writeToBuffer(frameIndex, &uboData, sizeof(CSMUBO));
}

std::optional<CascadeData> CSMManager::getCascadeData(CascadeIndex index) const {
    if (!index.isValid() || index.id >= kNumCascades) {
        return std::nullopt;
    }
    return cascadeData[index.id];
}

glm::mat4 CSMManager::getLightSpaceMatrix(CascadeIndex index) const {
    if (!index.isValid() || index.id >= kNumCascades) {
        return glm::mat4(1.0f);
    }
    return cascadeData[index.id].viewProj;
}

float CSMManager::getSplitDepth(CascadeIndex index) const {
    if (!index.isValid() || index.id >= kNumCascades) {
        return 0.0f;
    }
    return cascadeData[index.id].splitFar;
}

VkImageView CSMManager::getCascadeImageView(CascadeIndex index) const {
    if (!index.isValid() || index.id >= kNumCascades) {
        return VK_NULL_HANDLE;
    }
    return cascadeDepthImages[index.id] ? cascadeDepthImages[index.id]->getImageView() : VK_NULL_HANDLE;
}

std::array<VkImageView, CSMManager::kNumCascades> CSMManager::getCascadeImageViews() const {
    std::array<VkImageView, kNumCascades> views{};
    for (uint32_t i = 0; i < kNumCascades; ++i) {
        views[i] = cascadeDepthImages[i] ? cascadeDepthImages[i]->getImageView() : VK_NULL_HANDLE;
    }
    return views;
}

} // namespace ohao
