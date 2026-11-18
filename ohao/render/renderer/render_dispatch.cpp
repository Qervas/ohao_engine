#include "vulkan_renderer_impl.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include "render/camera/camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include "render/deferred/capture_preview_pass.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/frame/preview_extent.hpp"
#include "render/scene/render_scene.hpp"
#include "core/log.hpp"

#include <chrono>
#include <utility>

namespace ohao {

namespace {

// The stage a frame's submit waits for its swapchain image at (the acquire semaphore): the image's first
// barrier of the frame has it as its source, so the layout change waits for the acquire too.
constexpr VkPipelineStageFlags kAcquireWaitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
static_assert(VkPipelineStageFlags2{kAcquireWaitStage} == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);

// Whether a staging buffer holds an RGBA8 image of `extent`.
bool holdsRGBA8(const GpuBuffer& staging, VkExtent2D extent) {
    return staging.isValid() && staging.sizeBytes() >= static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
}

} // namespace

void VulkanRenderer::finishReadbackRequest(bool copied) noexcept {
    if (!copied) return;
    m_readbackReady = true;
    m_readbackSlot = m_currentFrame;
    ++m_requestedReadbacks;
}

uint32_t VulkanRenderer::framesInFlight() {
    if (!m_initialized) return 0;
    uint32_t n = 0;
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        n += vkGetFenceStatus(m_device, m_frameResources.getFrame(s).renderFence) == VK_NOT_READY ? 1u : 0u;
    return n;
}

void VulkanRenderer::waitForSubmitted(uint64_t n) {
    if (n == 0 || !m_initialized) return;
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        if (m_slotSubmitted[s] == n) m_frameResources.waitForFrame(s);
}

VulkanRenderer::Preview VulkanRenderer::readbackPreview() {
    if (!m_readbackReady || m_previewWidth == 0) return {};
    const void* mapped = m_staging[m_readbackSlot].getMappedData();
    if (!mapped || !m_frameResources.waitForFrame(m_readbackSlot)) return {};
    const std::size_t bytes = static_cast<std::size_t>(m_previewWidth) * m_previewHeight * 4;
    return {{static_cast<const uint8_t*>(mapped), bytes}, m_previewWidth, m_previewHeight};
}

bool VulkanRenderer::recordPreview(const render::Frame& frame, const PreviewRequest& request) {
    gpu::Image* const final = m_deferredRenderer ? m_deferredRenderer->finalOutput() : nullptr;
    if (!m_previewPass || !final) return false;
    GpuBuffer& staging = m_staging[m_currentFrame];
    const auto& rect = request.rect;
    if (!staging.isValid() || rect[2] == 0 || rect[3] == 0 || rect[0] + rect[2] > m_width ||
        rect[1] + rect[3] > m_height)
        return false;
    const auto [w, h] = render::previewExtent(rect[2], rect[3], request.maxSide);
    m_previewPass->record(frame, *final, rect, w, h, staging);
    m_previewWidth = w;
    m_previewHeight = h;
    return true;
}

std::span<const uint8_t> VulkanRenderer::readback() {
    if (!m_readbackReady || m_previewWidth != 0) return {};
    const void* mapped = m_staging[m_readbackSlot].getMappedData();
    if (!mapped || !m_frameResources.waitForFrame(m_readbackSlot)) return {};
    const std::size_t bytes = static_cast<std::size_t>(m_width) * m_height * 4;
    // A path-traced frame under a CPU-side denoiser: its images are final now that its
    // fence signalled, and nothing has been recorded since (the span would be empty).
    if (isRTRenderMode(m_renderMode) && m_denoiseMode == DenoiseMode::OIDN &&
        denoiseRequestedFrame() && m_denoisedPixelBuffer.size() == bytes)
        return {m_denoisedPixelBuffer.data(), bytes};
    return {static_cast<const uint8_t*>(mapped), bytes};
}

void VulkanRenderer::renderDeferred() {
    if (!m_deferredRenderer) return;
    log::setFrame(++m_framesStarted);
    gpu::setFaultFrame(m_framesStarted);
    // A request belongs to this frame; a frame that is not rendered leaves no readback.
    const bool wantReadback = std::exchange(m_readbackRequested, false);
    const std::optional<PreviewRequest> preview = std::exchange(m_previewRequest, std::nullopt);
    m_readbackReady = false;
    m_previewWidth = m_previewHeight = 0;

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
        if (acq == Swapchain::Acquire::Timeout) return;   // the presentation engine holds every image: skip
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
    if (m_textureManager) m_textureManager->onFrameRetired();
    m_allocator.collect();
    m_frameResources.resetFrame(m_currentFrame);
    forEachRTRenderer([](IRTRendererProfile& r) { r.releaseTargets(); });   // idle while the deferred frame draws

    // Update camera data
    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();
    glm::vec3 camPos = m_camera->getPosition();

    m_deferredRenderer->setRenderScene(m_renderScene.get());
    m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);
    m_deferredRenderer->setClusterFarPlane(m_camera->getFarPlane());
    m_deferredRenderer->setFrameNumber(m_framesStarted);
    m_deferredRenderer->setQuality(m_renderQuality);

    // RT direct light and GI trace the RenderScene's TLAS (DeferredRenderer reads it); RT direct
    // light samples the emissive triangle table and reads their textures. Without it, the
    // lighting loop and CSM.
    m_deferredRenderer->setRTDirectEnabled(isRTSupported() && m_rtDirectWanted);
    m_deferredRenderer->setEmissiveLights(m_emissiveLights.get());
    if (m_emissiveLights) {
        collectRTTextures();
        m_deferredRenderer->setBindlessTextures(m_rtTextureViews, m_rtTextureSamplers);
    }

    // The environment: RT GI's misses read the source image; lighting's IBL its prefiltered
    // levels and irradiance.
    if (envMapLoaded()) {
        const bool ibl = m_envPrefilter && m_envPrefilter->ready();
        m_deferredRenderer->setEnvMap(&m_envMap, ibl ? &m_envPrefilter->prefiltered() : nullptr,
                                      ibl ? &m_envPrefilter->irradiance() : nullptr,
                                      ibl ? m_envPrefilter->sampler() : VK_NULL_HANDLE, m_envIntensityScale);
    } else {
        m_deferredRenderer->setEnvMap(nullptr, nullptr, nullptr, VK_NULL_HANDLE, m_envIntensityScale);
    }

    // This frame's raster lights: the scene's light table. The deferred renderer writes them
    // into this slot's light buffer (its fence was waited on above), only when they changed.
    setFrameLights();

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    m_passTimer.beginFrame(cmd, m_currentFrame, m_framesStarted);
    const render::Frame recording{cmd, &m_passTimer, m_currentFrame};
    // A spot light's mesh shines toward this frame's camera as the light does (its cone). Before anything is
    // recorded: a light's first emitter mesh is a load, which the queue runs before this frame.
    if (m_camera) m_renderScene->setViewerPosition(m_camera->getPosition());
    // Skinned poses first: the TLAS build in recordUpdates reads the refit BLASes.
    m_renderScene->recordSkinning(recording);
    {
        render::Timed timed(recording, "scene.upload");
        m_renderScene->recordUpdates(recording);
    }

    // Execute deferred rendering pipeline (it times its own passes)
    m_deferredRenderer->render(cmd, m_currentFrame);

    // A preview request: only the preview reaches the staging buffer (capture.preview).
    const bool previewed = wantReadback && preview && recordPreview(recording, *preview);
    const bool copyFull = wantReadback && !previewed;

    // A window frame presents (and copies too when asked); a headless frame copies only
    // when asked, and otherwise ends with its final image.
    bool copied = previewed;
    if (m_swapchain) {
        render::Timed timed(recording, "present");
        copied = blitOutputToSwapchain(recording, imageIndex, m_deferredRenderer->finalOutput(),
                                       copyFull ? &m_staging[m_currentFrame] : nullptr) ||
                 previewed;
    } else if (copyFull) {
        render::Timed timed(recording, "readback");
        copied = copyOutputToStaging(recording, m_deferredRenderer->finalOutput());
    }
    if (wantReadback) finishReadbackRequest(copied);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const VkPipelineStageFlags waitStage = kAcquireWaitStage;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    if (m_swapchain) {
        renderFinished = m_swapchain->renderFinished(imageIndex);
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAcquired[m_currentFrame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
    }
    gpu::submit(m_graphicsQueue, submitInfo, frame.renderFence, "frame", m_framesStarted);
    m_slotSubmitted[m_currentFrame] = ++m_framesSubmitted;
    m_lastSubmitAt = std::chrono::steady_clock::now();
    m_passTimer.endFrame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());

    if (m_swapchain) {
        const VkResult pr = presentFrame(imageIndex);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    }

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

bool VulkanRenderer::blitOutputToSwapchain(const render::Frame& frame, uint32_t imageIndex, gpu::Image* src,
                                           GpuBuffer* staging) {
    if (src && !*src) src = nullptr;
    gpu::Image& dst = m_swapchainImages[imageIndex];
    // Acquired for this frame: whatever it held is not wanted (so its first use is from UNDEFINED, as after a
    // rebuild), and it is this frame's from the stage the submit waits for the acquire at.
    dst.assume(VK_IMAGE_LAYOUT_UNDEFINED, kAcquireWaitStage, 0);

    // A requested readback: the same image, already a transfer source, into the frame's staging
    // buffer. Never on a frame nobody asked for.
    const bool copy = src && staging && holdsRGBA8(*staging, src->extent());
    {
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        if (src) pass.copySrc(*src);
        pass.copyDst(dst);
        if (copy) pass.write(*staging, render::BufferUse::CopyDst);
        pass.begin();
        if (src) {
            const VkExtent2D from = src->extent();
            const VkExtent2D to = m_swapchain->extent();
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1]  = {static_cast<int32_t>(from.width), static_cast<int32_t>(from.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1]  = {static_cast<int32_t>(to.width), static_cast<int32_t>(to.height), 1};
            vkCmdBlitImage(frame.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            if (copy) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {from.width, from.height, 1};
                vkCmdCopyImageToBuffer(frame.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1,
                                       &region);
            }
        } else {
            // No output this frame: present black rather than garbage.
            const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(frame.cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        }
    }

    render::Pass out(frame, nullptr, render::Pass::Transfer);
    if (copy) out.read(*staging, render::BufferUse::HostRead);   // visible to the host once the frame's fence signals
    out.present(dst);
    out.begin();
    return copy;
}

void VulkanRenderer::renderRTPipeline(const RTRenderSettings& defaults) {
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer || !isRTSupported()) return;
    log::setFrame(++m_framesStarted);
    gpu::setFaultFrame(m_framesStarted);
    // A preview request reads back in full here: the path tracer has no capture.preview.
    const bool wantReadback = std::exchange(m_readbackRequested, false);
    m_previewRequest.reset();
    m_readbackReady = false;
    m_previewWidth = m_previewHeight = 0;

    // A window: the swapchain as the deferred path handles it (rebuilt before the slot is
    // taken, an image acquired before the slot's fence is reset).
    if (m_swapchain && (m_swapchainDirty || !m_swapchain->valid())) {
        if (!recreatePresentation()) return;   // minimized: skip the frame
    }

    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    uint32_t imageIndex = 0;
    if (m_swapchain) {
        const auto acq = m_swapchain->acquire(m_imageAcquired[m_currentFrame], imageIndex);
        if (acq == Swapchain::Acquire::OutOfDate) { m_swapchainDirty = true; return; }
        if (acq == Swapchain::Acquire::Timeout) return;   // the presentation engine holds every image: skip
        if (acq == Swapchain::Acquire::Error) {
            OHAO_LOG(Error, gpu, "present: vkAcquireNextImageKHR failed");
            return;
        }
    }

    const auto cpuStart = std::chrono::steady_clock::now();   // the frame's CPU time: record + submit
    m_renderScene->onFrameRetired();   // retire-queue tick, after the slot's fence wait
    if (m_textureManager) m_textureManager->onFrameRetired();
    m_allocator.collect();
    m_frameResources.resetFrame(m_currentFrame);
    forEachRTRenderer([&](IRTRendererProfile& r) {   // the other profile's tracer is idle
        if (&r != rtRenderer) r.releaseTargets();
    });

    glm::mat4 view = m_camera->getViewMatrix();
    glm::mat4 proj = m_camera->getProjectionMatrix();

    prepareRTSceneForFrame(defaults);

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    m_passTimer.beginFrame(cmd, m_currentFrame, m_framesStarted);
    const render::Frame recording{cmd, &m_passTimer, m_currentFrame};
    // A spot light's mesh shines toward this frame's camera as the light does (its cone). Before anything is
    // recorded: a light's first emitter mesh is a load, which the queue runs before this frame.
    if (m_camera) m_renderScene->setViewerPosition(m_camera->getPosition());
    m_renderScene->recordSkinning(recording);
    {
        render::Timed timed(recording, "scene.upload");
        m_renderScene->recordUpdates(recording);
    }
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
    // The tracer times its own stages (emissive.lights, pt.cache.update ... pt.denoise).
    if (const auto* pp = m_deferredRenderer ? m_deferredRenderer->getPostProcessing() : nullptr)
        rtRenderer->setDisplay(render::displayExposure(pp->ev100()), pp->displayCurve());
    rtRenderer->render(recording, ptView, ptProj);

    // The path tracer's output (RGBA8, through the display transform): a window blits it into the
    // swapchain image, with the requested readback copy inside the same pass; a headless frame
    // copies it only when asked.
    gpu::Image& ptOutput = rtRenderer->image(PathTracer::Target::Output);
    GpuBuffer* const staging = wantReadback ? &m_staging[m_currentFrame] : nullptr;
    if (m_swapchain) {
        render::Timed timed(recording, "pt.present");
        const bool copied = blitOutputToSwapchain(recording, imageIndex, &ptOutput, staging);
        if (wantReadback) finishReadbackRequest(copied);
    } else if (staging) {
        render::Timed timed(recording, "pt.readback");
        if (copyOutputToStaging(recording, &ptOutput)) finishReadbackRequest(true);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const VkPipelineStageFlags waitStage = kAcquireWaitStage;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    if (m_swapchain) {   // the same per-slot semaphores as the deferred path
        renderFinished = m_swapchain->renderFinished(imageIndex);
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAcquired[m_currentFrame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
    }
    gpu::submit(m_graphicsQueue, submitInfo, frame.renderFence, "frame", m_framesStarted);
    m_slotSubmitted[m_currentFrame] = ++m_framesSubmitted;
    m_lastSubmitAt = std::chrono::steady_clock::now();
    m_passTimer.endFrame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count());

    if (m_swapchain) {
        const VkResult pr = presentFrame(imageIndex);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    }

    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}

bool VulkanRenderer::copyOutputToStaging(const render::Frame& frame, gpu::Image* src) {
    if (src && !*src) src = nullptr;
    GpuBuffer& staging = m_staging[m_currentFrame];
    if (!src || !holdsRGBA8(staging, src->extent())) return false;

    {
        render::Pass pass(frame, nullptr, render::Pass::Transfer);
        pass.copySrc(*src).write(staging, render::BufferUse::CopyDst);
        pass.begin();
        const VkExtent2D from = src->extent();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {from.width, from.height, 1};
        vkCmdCopyImageToBuffer(frame.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    }
    // Visible to the host once the frame's fence signals.
    render::Pass toHost(frame, nullptr, render::Pass::Transfer);
    toHost.read(staging, render::BufferUse::HostRead);
    toHost.begin();
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
            destroyStagingBuffers();
            if (!createStagingBuffers())
                OHAO_LOG(Error, gpu, "the readback staging buffers could not be resized to {}x{}", width, height);
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
    auto* rtRenderer = getRTRenderer(m_renderMode);
    if (!rtRenderer) return false;
    w = m_width; h = m_height;

    vkDeviceWaitIdle(m_device);
    bool ok = readImageRGBA(rtRenderer->accum(), beauty);
    ok &= readImageRGBA(rtRenderer->image(PathTracer::Target::Albedo), albedo);
    ok &= readImageRGBA(rtRenderer->image(PathTracer::Target::Normal), normal);
    return ok;
}

std::vector<float> VulkanRenderer::readbackLinear() {
    std::vector<float> out;
    vkDeviceWaitIdle(m_device);
    if (isRTRenderMode(m_renderMode)) {
        auto* rt = getRTRenderer(m_renderMode);
        if (!rt || !readImageRGBA(rt->accum(), out)) out.clear();
    } else if (m_deferredRenderer && m_deferredRenderer->renderScale() == 1.0f) {   // the lit image is the frame's size
        gpu::Image* const lit = m_deferredRenderer->litImage();
        if (!lit || !readImageRGBA(*lit, out)) out.clear();
    }
    return out;
}

bool VulkanRenderer::readImage(gpu::Image& image, uint32_t texelBytes, void* out) {
    const VkExtent2D extent = image.extent();
    if (!image || m_width == 0 || m_height == 0 || extent.width != m_width || extent.height != m_height) return false;
    const VkDeviceSize size = static_cast<VkDeviceSize>(m_width) * m_height * texelBytes;
    GpuBuffer staging = m_allocator.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, AllocationUsage::GpuToCpu, true);
    if (!staging.isValid() || !staging.getMappedData()) {
        m_allocator.destroyBuffer(staging);
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = m_commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    {
        // The image stays a transfer source: its next pass moves it from there.
        render::Pass pass(render::Frame{cmd, nullptr, 0}, nullptr, render::Pass::Transfer);
        pass.copySrc(image).write(staging, render::BufferUse::CopyDst);
        pass.begin();
        VkBufferImageCopy region{};
        region.imageSubresource = {image.aspect(), 0, 0, 1};
        region.imageExtent = {m_width, m_height, 1};
        vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    gpu::submitAndWait(m_device, m_graphicsQueue, si, "image readback");

    m_allocator.invalidateBuffer(staging);
    std::memcpy(out, staging.getMappedData(), static_cast<std::size_t>(size));
    m_allocator.destroyBuffer(staging);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    return true;
}

bool VulkanRenderer::readImageRGBA(gpu::Image& image, std::vector<float>& out) {
    const std::size_t values = static_cast<std::size_t>(m_width) * m_height * 4;
    out.resize(values);
    if (image.desc.format != VK_FORMAT_R16G16B16A16_SFLOAT) return readImage(image, 4 * sizeof(float), out.data());
    std::vector<uint16_t> halves(values);
    if (!readImage(image, 4 * sizeof(uint16_t), halves.data())) return false;
    for (std::size_t i = 0; i < values; ++i) out[i] = glm::unpackHalf1x16(halves[i]);
    return true;
}

} // namespace ohao
