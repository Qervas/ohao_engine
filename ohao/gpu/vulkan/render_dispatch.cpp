#include "renderer_impl.hpp"
#include "render/camera/camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "render/deferred/deferred_renderer.hpp"
#include "render/rt/rt_visibility.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <chrono>
#include <utility>

namespace ohao {

namespace {

// The fallback sun's angular radius for its RT shadow penumbra: the real sun's, 0.267 deg.
constexpr float kSunAngularRadius = 0.00465f;

// The copy into a staging buffer, visible to the host once the frame's fence signals.
void stagingToHost(VkCommandBuffer cmd, VkBuffer staging) {
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = staging;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0,
                         nullptr);
}

} // namespace

void VulkanRenderer::finishReadbackRequest(bool copied) noexcept {
    if (!copied) return;
    m_readbackReady = true;
    m_readbackSlot = m_currentFrame;
    ++m_requestedReadbacks;
}

std::span<const uint8_t> VulkanRenderer::readback() {
    if (!m_readbackReady) return {};
    const FrameResources& f = m_frameResources.getFrame(m_readbackSlot);
    if (!f.stagingBufferMapped || !m_frameResources.waitForFrame(m_readbackSlot)) return {};
    const std::size_t bytes = static_cast<std::size_t>(m_width) * m_height * 4;
    // A path-traced frame under a CPU-side denoiser: its images are final now that its
    // fence signalled, and nothing has been recorded since (the span would be empty).
    if (isRTRenderMode(m_renderMode) && (m_denoiseMode == DenoiseMode::OIDN || m_denoiseMode == DenoiseMode::NRD) &&
        denoiseRequestedFrame() && m_denoisedPixelBuffer.size() == bytes)
        return {m_denoisedPixelBuffer.data(), bytes};
    return {static_cast<const uint8_t*>(f.stagingBufferMapped), bytes};
}

void VulkanRenderer::renderDeferred() {
    if (!m_deferredRenderer) return;
    log::setFrame(++m_framesStarted);
    // A request belongs to this frame; a frame that is not rendered leaves no readback.
    const bool wantReadback = std::exchange(m_readbackRequested, false);
    m_readbackReady = false;

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
            OHAO_LOG(Error, gpu, "present: vkAcquireNextImageKHR failed");
            return;
        }
    }

    // Retire-queue tick. After the early returns above: every tick then matches a
    // frame that is submitted on this slot, so MAX_FRAMES_IN_FLIGHT ticks cover
    // every slot's fence.
    const auto cpuStart = std::chrono::steady_clock::now();   // the frame's CPU time: record + submit
    m_renderScene->onFrameRetired();
    m_frameResources.resetFrame(m_currentFrame);

    // Update camera data
    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();
    glm::vec3 camPos = m_camera->getPosition();

    m_deferredRenderer->setRenderScene(m_renderScene.get());
    m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);
    m_deferredRenderer->setFrameNumber(m_framesStarted);
    m_deferredRenderer->setQuality(m_renderQuality);

    // RT shadows and GI trace the RenderScene's TLAS (DeferredRenderer reads it); without
    // RT shadows, CSM.
    m_deferredRenderer->setRTShadowsEnabled(isRTSupported() && m_rtShadowsWanted);

    // Pass env map to deferred renderer for reflections
    if (envMapLoaded()) {
        m_deferredRenderer->setEnvMap(m_envMapImageView, m_envSampler);
    }

    // This slot's light UBO (its fence was waited on above): scene lights, or the
    // default sun when the scene has none.
    LightUniformBuffer* ld = updateLightBuffer(m_currentFrame);

    // Sync fallback directional light with sky sun direction so deferred lighting matches the sky
    if (ld) {
        // Only the fallback sun: a scene with exactly one light of its own keeps it (and its
        // ambient) as the scene set them.
        if (m_renderScene->rasterLights().empty()) {
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

    if (ld && m_ambientOverride) ld->ambientIntensity = *m_ambientOverride;

    // The lighting pass binds this slot's buffer in this slot's descriptor set.
    if (ld) {
        const uint32_t lightCount = static_cast<uint32_t>(std::max(1, ld->numLights));
        m_deferredRenderer->setLightBuffer(m_lightBuffers[m_currentFrame], lightCount);
    }

    // The lights RT shadows trace: the scene's shadow casters, or the fallback sun (its
    // params.w names channel 0) when the scene has no lights.
    if (!m_renderScene->rasterLights().empty()) {
        m_deferredRenderer->setShadowLights(m_renderScene->shadowLights());
    } else if (ld && ld->lights[0].params.w == 0.0f) {
        render::LightDesc sun{};
        sun.kind = render::LightKind::Directional;
        sun.direction = glm::vec3(ld->lights[0].direction);
        sun.radius = kSunAngularRadius;
        m_deferredRenderer->setShadowLights(std::span<const render::LightDesc>(&sun, 1));
    } else {
        m_deferredRenderer->setShadowLights({});
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    m_passTimer.beginFrame(cmd, m_currentFrame, m_framesStarted);
    m_passTimer.begin(cmd, "scene.upload");
    m_renderScene->recordUpdates(cmd, m_currentFrame);
    m_passTimer.end(cmd);

    // Execute deferred rendering pipeline (it times its own passes)
    m_deferredRenderer->render(cmd, m_currentFrame);

    // A window frame presents (and copies too when asked); a headless frame copies only
    // when asked, and otherwise ends with its final image.
    bool copied = false;
    if (m_swapchain) {
        m_passTimer.begin(cmd, "present");
        copied = blitDeferredOutputToSwapchain(cmd, imageIndex, wantReadback ? frame.stagingBuffer : VK_NULL_HANDLE);
        m_passTimer.end(cmd);
    } else if (wantReadback) {
        m_passTimer.begin(cmd, "readback");
        copied = copyDeferredOutputToStaging(cmd);
        m_passTimer.end(cmd);
    }
    if (wantReadback) finishReadbackRequest(copied);

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
    m_passTimer.endFrame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());

    if (m_swapchain) {
        const VkResult pr = m_swapchain->present(m_graphicsQueue, imageIndex);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    }

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

bool VulkanRenderer::blitDeferredOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex, VkBuffer staging) {
    const VkImage src = m_deferredRenderer->getFinalOutputImage();
    bool copied = false;
    const VkImage dst = m_swapchain->image(imageIndex);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier b[2]{};
    for (auto& x : b) {
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.subresourceRange = range;
    }
    b[0].image = src;    // same layout assumption copyDeferredOutputToStaging makes
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
        // A requested readback: the same image, already a transfer source, into the
        // frame's staging buffer. Never on a frame nobody asked for.
        if (staging != VK_NULL_HANDLE && m_deferredRenderer->finalOutputIsRGBA8()) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {m_width, m_height, 1};
            vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
            stagingToHost(cmd, staging);
            copied = true;
        }
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
    return copied;
}

void VulkanRenderer::renderRTPipeline(const IRTRenderPipeline& pipeline) {
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer || !isRTSupported()) return;
    log::setFrame(++m_framesStarted);
    const bool wantReadback = std::exchange(m_readbackRequested, false);
    m_readbackReady = false;

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    const auto cpuStart = std::chrono::steady_clock::now();   // the frame's CPU time: record + submit
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
    m_passTimer.beginFrame(cmd, m_currentFrame, m_framesStarted);
    m_passTimer.begin(cmd, "scene.upload");
    m_renderScene->recordUpdates(cmd, m_currentFrame);
    m_passTimer.end(cmd);
    // The traced scene changed (TLAS built or refit, instance data or lights
    // uploaded): treat it like a view change. Accumulating profiles restart; the
    // realtime profile flags the frame and keeps its denoiser history. A still
    // scene uploads nothing and keeps accumulating.
    if (const auto& st = m_renderScene->stats();
        st.lastTlasOp != render::RenderSceneStats::TlasOp::None || st.instanceBytesUploaded > 0 ||
        st.lightBytesUploaded > 0) {
        rtRenderer->notifyViewChanged();
    }
    // After recordUpdates: table growth and TLAS (re)creation replace handles.
    rtRenderer->setSceneBuffers(*m_renderScene);

    // PT needs unflipped projection (ray generation handles Y internally).
    // Use same FOV/aspect as deferred for consistent framing.
    float aspect = float(m_width) / float(m_height);
    glm::mat4 ptView = m_camera->getViewMatrix();
    glm::mat4 ptProj = glm::perspective(glm::radians(m_camera->getFov()), aspect, 0.1f, 1000.0f);

    // Lit by the scene's lights: the RenderScene light table (setSceneBuffers above).
    m_passTimer.begin(cmd, "pt.render");
    rtRenderer->render(cmd, m_currentFrame, ptView, ptProj);
    m_passTimer.end(cmd);

    // The path tracer's output into the staging buffer, for a requested frame only.
    VkImage ptOutput = rtRenderer->getOutputImage();
    if (wantReadback && ptOutput != VK_NULL_HANDLE && frame.stagingBuffer != VK_NULL_HANDLE) {
        m_passTimer.begin(cmd, "pt.readback");
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
        stagingToHost(cmd, frame.stagingBuffer);
        m_passTimer.end(cmd);
        finishReadbackRequest(true);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);
    m_passTimer.endFrame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

bool VulkanRenderer::copyDeferredOutputToStaging(VkCommandBuffer cmd) {
    if (!m_deferredRenderer) return false;

    VkImage finalImage = m_deferredRenderer->getFinalOutputImage();
    if (finalImage == VK_NULL_HANDLE) return false;
    // The staging buffer holds width*height*4 bytes. An RGBA16F final image (post stack
    // off) is twice that: a straight copy would write past the buffer. Refuse instead.
    if (!m_deferredRenderer->finalOutputIsRGBA8()) {
        static bool warned = false;
        if (!warned) {
            OHAO_LOG(Warn, gpu, "deferred readback skipped: tonemapping is off, the final image is HDR");
            warned = true;
        }
        return false;
    }

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    if (frame.stagingBuffer == VK_NULL_HANDLE) return false;

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
    stagingToHost(cmd, frame.stagingBuffer);

    // Back to where the frame left it, as the window's blit does: a frame ends in the same
    // state whether or not it was read.
    VkImageMemoryBarrier back = toTransferSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &back);
    return true;
}

void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;                 // minimized: keep the old targets
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    if (m_camera) m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));

    if (m_initialized) {
        vkDeviceWaitIdle(m_device);
        // Every slot's frame has finished: read their timings before m_currentFrame resets.
        m_passTimer.collectAll([](uint32_t) { return true; });

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
