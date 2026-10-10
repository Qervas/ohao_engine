#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "scene/component/mesh_component.hpp"
#include "scene/asset/model.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "scene/scene.hpp"
#include "scene/actor/actor.hpp"
#include "render/rt/rt_visibility.hpp"
#include "render/scene/render_scene.hpp"

namespace ohao {

// Render mode implementations
// Split from offscreen_renderer.cpp

void VulkanRenderer::renderDeferred() {
    if (!m_deferredRenderer) return;

    // Rebuild the swapchain BEFORE taking a frame slot. Recreating resizes the
    // renderer, which idles the device and resets m_currentFrame; doing it after
    // the slot was chosen would wait on one slot's fence and reset another's.
    if (m_swapchain && (m_swapchainDirty || !m_swapchain->valid())) {
        if (!recreatePresentation()) return;   // minimized: skip the frame
    }

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    // Every early return below happens BEFORE resetFrame, so the slot's fence stays
    // signaled and the next waitForFrame on it cannot deadlock.
    uint32_t imageIndex = 0;
    if (m_swapchain) {
        const auto acq = m_swapchain->acquire(m_imageAcquired[m_currentFrame], imageIndex);
        if (acq == Swapchain::Acquire::OutOfDate) { m_swapchainDirty = true; return; }
        if (acq == Swapchain::Acquire::Error) {
            std::cerr << "[present] vkAcquireNextImageKHR failed" << std::endl;
            return;
        }
    } else if (frame.stagingBufferMapped) {
        // Headless: this slot's staging buffer holds the frame finished
        // MAX_FRAMES_IN_FLIGHT frames ago, complete now that its fence signaled.
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped,
               static_cast<size_t>(m_width) * m_height * 4);
    }

    // Retire-queue tick. After the early returns above: every tick then matches a
    // frame that is submitted on this slot, so MAX_FRAMES_IN_FLIGHT ticks cover
    // every slot's fence.
    m_renderScene->onFrameRetired();
    m_frameResources.resetFrame(m_currentFrame);

    // Update camera data
    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();
    glm::vec3 camPos = m_camera->getPosition();

    m_deferredRenderer->setScene(m_scene);   // RT GI's bounce light still reads it
    m_deferredRenderer->setRenderScene(m_renderScene.get());
    m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);

    // Skeletal animation removed — all meshes are static.

    // RT shadows and GI trace the RenderScene's TLAS (DeferredRenderer reads it).
    if (isRTSupported()) {
        m_deferredRenderer->setRTShadowsEnabled(true);
    }

    // Pass env map to deferred renderer for reflections
    if (envMapLoaded()) {
        m_deferredRenderer->setEnvMap(m_envMapImageView, m_envSampler);
    }

    // TEMPORARY bridge: the old Scene's live transforms, visibility and lights into
    // the RenderScene, before recordUpdates uploads them.
    syncBridgedScene();

    // Update m_lightBuffer with current scene lights (correct numLights, default fallback if empty)
    updateLightBuffer();

    // Sync fallback directional light with sky sun direction so deferred lighting matches the sky
    if (m_lightBufferMapped && m_deferredRenderer) {
        LightUniformBuffer* ld = static_cast<LightUniformBuffer*>(m_lightBufferMapped);
        if (ld && ld->numLights == 1) {
            // No explicit scene lights — align the fallback sun with sky
            glm::vec3 skyLightDir = m_deferredRenderer->getLightDirection(); // points DOWN toward scene
            float nightFactor = m_deferredRenderer->getNightFactor();

            if (glm::length(skyLightDir) > 0.001f) {
                if (nightFactor > 0.01f) {
                    // Blend light direction from sun toward moon at night
                    glm::vec3 moonDir = -m_deferredRenderer->getMoonDirection();
                    glm::vec3 blendedDir = glm::normalize(glm::mix(
                        glm::normalize(skyLightDir), moonDir, nightFactor));
                    ld->lights[0].direction = glm::vec4(blendedDir, 100.0f);

                    // Blend color from warm white (sun) to cool blue (moon)
                    glm::vec3 sunColor  = glm::vec3(1.0f, 0.98f, 0.95f);
                    glm::vec3 moonColor = glm::vec3(0.6f, 0.7f, 0.85f);
                    glm::vec3 blendedColor = glm::mix(sunColor, moonColor, nightFactor);

                    // Blend intensity from full (PI) to ~8% moonlight
                    float sunIntensity  = glm::pi<float>();
                    float moonIntensity = glm::pi<float>() * 0.08f;
                    float blendedIntensity = glm::mix(sunIntensity, moonIntensity, nightFactor);

                    ld->lights[0].color = glm::vec4(blendedColor, blendedIntensity);
                } else {
                    ld->lights[0].direction = glm::vec4(glm::normalize(skyLightDir), 100.0f);
                }
                ld->lights[0].lightSpaceMatrix = calculateLightSpaceMatrix(ld->lights[0]);
            }
            // Ambient: moderate during day, lower at night
            ld->ambientIntensity = glm::mix(0.12f, 0.04f, nightFactor);
        }
    }

    // Pass actual light count to deferred renderer (avoids NaN from zero-init garbage entries)
    if (m_lightBuffer != VK_NULL_HANDLE && m_lightBufferMapped) {
        LightUniformBuffer* lightData = static_cast<LightUniformBuffer*>(m_lightBufferMapped);
        uint32_t lightCount = lightData ? static_cast<uint32_t>(std::max(1, lightData->numLights)) : 1u;
        m_deferredRenderer->setLightBuffer(m_lightBuffer, lightCount);
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    m_renderScene->recordUpdates(cmd, m_currentFrame);

    // Execute deferred rendering pipeline
    m_deferredRenderer->render(cmd, m_currentFrame);

    if (m_swapchain) blitDeferredOutputToSwapchain(cmd, imageIndex);
    else             copyDeferredOutputToPixelBuffer(cmd);   // headless readback

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    if (m_swapchain) {
        renderFinished = m_swapchain->renderFinished(imageIndex);
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAcquired[m_currentFrame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
    }
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    if (m_swapchain) {
        const VkResult pr = m_swapchain->present(m_graphicsQueue, imageIndex);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    }

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void VulkanRenderer::blitDeferredOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex) {
    const VkImage src = m_deferredRenderer->getFinalOutputImage();
    const VkImage dst = m_swapchain->image(imageIndex);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier b[2]{};
    for (auto& x : b) {
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.subresourceRange = range;
    }
    b[0].image = src;    // same layout assumption copyDeferredOutputToPixelBuffer makes
    b[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[1].image = dst;
    b[1].srcAccessMask = 0;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const uint32_t count = src != VK_NULL_HANDLE ? 2u : 1u;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        count, src != VK_NULL_HANDLE ? b : &b[1]);

    if (src != VK_NULL_HANDLE) {
        const VkExtent2D ext = m_swapchain->extent();
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1]  = {static_cast<int32_t>(m_width), static_cast<int32_t>(m_height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1]  = {static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    } else {
        // No deferred output this frame: present black rather than garbage.
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }

    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = 0;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, count, src != VK_NULL_HANDLE ? b : &b[1]);
}

void VulkanRenderer::renderRTPipeline(const IRTRenderPipeline& pipeline) {
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer || !isRTSupported()) return;

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    // Read back previous frame's pixels
    if (frame.stagingBufferMapped) {
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped, m_width * m_height * 4);
    }

    m_renderScene->onFrameRetired();   // retire-queue tick, after the slot's fence wait
    m_frameResources.resetFrame(m_currentFrame);

    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();

    prepareRTSceneForFrame(pipeline);

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    m_renderScene->recordUpdates(cmd, m_currentFrame);
    // After recordUpdates: table growth and TLAS (re)creation replace handles.
    rtRenderer->setSceneBuffers(*m_renderScene);

    // PT needs unflipped projection (ray generation handles Y internally).
    // Use same FOV/aspect as deferred for consistent framing.
    float aspect = float(m_width) / float(m_height);
    glm::mat4 ptView = m_camera->getViewMatrix();
    glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);

    // Sphere light: position, intensity, color, radius
    rtRenderer->render(cmd, ptView, ptProj,
                       glm::vec3(0.0f, 4.0f, 0.0f), 30.0f,
                       glm::vec3(1.0f, 0.98f, 0.92f), 1.0f);

    // Copy path tracer output to staging buffer for CPU readback
    VkImage ptOutput = rtRenderer->getOutputImage();
    if (ptOutput != VK_NULL_HANDLE && frame.stagingBuffer != VK_NULL_HANDLE) {
        // Output is already in TRANSFER_SRC_OPTIMAL from render()
        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {m_width, m_height, 1};

        vkCmdCopyImageToBuffer(cmd, ptOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               frame.stagingBuffer, 1, &copyRegion);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

void VulkanRenderer::copyDeferredOutputToPixelBuffer(VkCommandBuffer cmd) {
    if (!m_deferredRenderer) return;
    ++m_readbackCopies;

    VkImage finalImage = m_deferredRenderer->getFinalOutputImage();
    if (finalImage == VK_NULL_HANDLE) return;
    // The staging buffer holds width*height*4 bytes. An RGBA16F final image (post stack
    // off) is twice that: a straight copy would write past the buffer. Refuse instead.
    if (!m_deferredRenderer->finalOutputIsRGBA8()) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "[ohao] deferred readback skipped: tonemapping is off, the final image is HDR\n";
            warned = true;
        }
        return;
    }

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    if (frame.stagingBuffer == VK_NULL_HANDLE) return;

    // Transition final output: SHADER_READ_ONLY_OPTIMAL -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier toTransferSrc{};
    toTransferSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.image = finalImage;
    toTransferSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransferSrc.subresourceRange.baseMipLevel = 0;
    toTransferSrc.subresourceRange.levelCount = 1;
    toTransferSrc.subresourceRange.baseArrayLayer = 0;
    toTransferSrc.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);

    // Copy image to staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, 1};

    vkCmdCopyImageToBuffer(cmd, finalImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.stagingBuffer, 1, &region);
}

void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;                 // minimized: keep the old targets
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    m_pixelBuffer.resize(static_cast<size_t>(width) * height * 4);
    if (m_camera) m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));

    if (m_initialized) {
        vkDeviceWaitIdle(m_device);

        // Resize multi-frame staging buffers (critical for correct pixel readback!)
        if (m_frameResources.isInitialized()) {
            m_frameResources.resizeStagingBuffers(static_cast<size_t>(width) * height * 4);
            // Reset frame index to avoid reading from old-size staging buffers
            m_currentFrame = 0;
        }

        // Resize deferred renderer
        if (m_deferredRenderer) {
            m_deferredRenderer->onResize(width, height);
        }

        // Resize RT path tracers (offline/realtime) to match framebuffer
        forEachRTRenderer([&](IRTRendererProfile& r) { r.resize(width, height); });
    }
}

bool VulkanRenderer::readTerrainHeights(std::vector<float>& outData, uint32_t& outRes) {
    return false; // Terrain pass disabled
}

bool VulkanRenderer::readbackHDRBuffers(std::vector<float>& beauty, std::vector<float>& albedo,
                                        std::vector<float>& normal, uint32_t& w, uint32_t& h) {
    const auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer) return false;
    w = m_width; h = m_height;

    auto readbackImage = [&](VkImage image, std::vector<float>& outBuf) -> bool {
        if (image == VK_NULL_HANDLE) return false;
        VkDeviceSize size = m_width * m_height * 4 * sizeof(float);  // RGBA32F

        // Create staging buffer
        VkBuffer staging; VkDeviceMemory stagingMem;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &staging);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, staging, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &stagingMem);
        vkBindBufferMemory(m_device, staging, stagingMem, 0);

        // Command buffer
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = m_commandPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Barrier: GENERAL → TRANSFER_SRC
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

        // Barrier back: TRANSFER_SRC → GENERAL
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);

        // Map and copy
        outBuf.resize(m_width * m_height * 4);
        void* mapped;
        vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
        memcpy(outBuf.data(), mapped, size);
        vkUnmapMemory(m_device, stagingMem);

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMem, nullptr);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return true;
    };

    vkDeviceWaitIdle(m_device);
    bool ok = readbackImage(rtRenderer->getAccumImage(), beauty);
    ok &= readbackImage(rtRenderer->getAlbedoAOV(), albedo);
    ok &= readbackImage(rtRenderer->getNormalAOV(), normal);
    return ok;
}


} // namespace ohao
