#include "vulkan_renderer_impl.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include "render/rt/env_cdf.hpp"
#include "render/rt/env_sun.hpp"
#include "render/scene/render_scene.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "stb_image.h"

#include "core/log.hpp"

#include <cstring>
#include <span>

namespace ohao {

// Environment map (HDRI) for the path tracer's miss shader and the deferred sky
// reflections. The image goes into the one bindless texture table; its index and
// the intensity scale are stamped into the RenderScene's RT light header.

namespace {

// The old RT texture sampler (uploadRTTextureArray): bilinear, repeat, base level
// only. The env map keeps it so the HDRI is sampled exactly as before.
VkSampler createEnvSampler(VkDevice device) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    info.compareEnable = VK_FALSE;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sampler;
}

} // namespace

std::string VulkanRenderer::environmentKey() const {
    if (!m_envMapPath.empty()) return "file:" + m_envMapPath + (m_envMapSunless ? "|sunless" : "");
    if (!m_envSky || !m_renderScene) return {};
    const auto sun = m_renderScene->sun();
    if (!sun) return {};   // a sky without a sun is black: no environment
    // The sun to 1e-5 in direction and illuminance: a moved or brightened sun re-renders the sky.
    char key[160];
    std::snprintf(key, sizeof(key), "sky:%.5f,%.5f,%.5f:%.5g,%.5g,%.5g", sun->toward.x, sun->toward.y, sun->toward.z,
                  sun->illuminance.x, sun->illuminance.y, sun->illuminance.z);
    return key;
}

void VulkanRenderer::uploadEnvironmentMap() {
    if (m_device == VK_NULL_HANDLE) return;
    if (m_envSampler == VK_NULL_HANDLE) m_envSampler = createEnvSampler(m_device);

    // Once per environment (reloading on every scene upload leaked multi-GB HDR images); none
    // unloads what was there (the queue is idle: render() waited).
    const std::string key = environmentKey();
    if (key.empty() && !m_envMapLoadedKey.empty()) destroyEnvMapImage();
    if (!key.empty() && key != m_envMapLoadedKey && m_textureManager) {
        int ew = 0, eh = 0, ec = 0;
        float* hdrPixels = nullptr;
        std::vector<float> skyPixels;   // the physical sky's map, when there is no HDRI
        if (!m_envMapPath.empty()) {
            hdrPixels = stbi_loadf(m_envMapPath.c_str(), &ew, &eh, &ec, 4);
        } else {
            // 512 x 256: the sky is smooth; its tables are built once.
            constexpr uint32_t kSkyW = 512, kSkyH = 256;
            if (!m_atmosphere) m_atmosphere = std::make_unique<render::Atmosphere>();
            const auto sun = m_renderScene->sun();
            skyPixels = m_atmosphere->equirect(kSkyW, kSkyH, sun->toward, sun->illuminance);
            ew = static_cast<int>(kSkyW);
            eh = static_cast<int>(kSkyH);
            hdrPixels = skyPixels.data();
            OHAO_LOG(Info, gpu, "env: the physical sky for the sun toward ({:.3f}, {:.3f}, {:.3f})", sun->toward.x,
                     sun->toward.y, sun->toward.z);
        }
        if (!hdrPixels) {
            OHAO_LOG(Error, gpu, "RT: env map: failed to load {}", m_envMapPath);
        } else {
            // A directional light stands for the sun: every consumer below (RT GI's rays, the
            // deferred IBL, the path tracer's misses and its importance sampling) lights with the
            // sky alone, so the sun is counted once and GI's cosine rays never meet it.
            if (m_envMapSunless && skyPixels.empty()) {   // the sky's map has no sun disc
                const render::EnvSun sun = render::separateEnvSun(
                    std::span<float>(hdrPixels, static_cast<size_t>(ew) * static_cast<size_t>(eh) * 4u), ew, eh);
                if (sun.found)
                    OHAO_LOG(Info, gpu,
                             "env map: a directional light stands for the sun; {} lights without its sun "
                             "({} texels, {:.2e} sr, {:.1f}% of its power, peak {:.0f} over a median sky of {:.3f}, "
                             "toward ({:.3f}, {:.3f}, {:.3f}))",
                             m_envMapPath, sun.texels, sun.solidAngle, sun.powerShare * 100.0f, sun.peak, sun.median,
                             sun.direction.x, sun.direction.y, sun.direction.z);
                else
                    OHAO_LOG(Info, gpu, "env map: {} has no sun to take out (peak {:.1f}, median sky {:.3f})",
                             m_envMapPath, sun.peak, sun.median);
            }
            // Replacing an earlier env map: frames in flight may still sample it.
            if (m_envMapImage != VK_NULL_HANDLE) {
                gpu::checkResult(vkQueueWaitIdle(m_graphicsQueue), "environment replace");
                destroyEnvMapImage();
            }

            VkImageCreateInfo imgInfo{};
            imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            imgInfo.extent = {static_cast<uint32_t>(ew), static_cast<uint32_t>(eh), 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage envImage = VK_NULL_HANDLE;
            VkDeviceMemory envMem = VK_NULL_HANDLE;
            bool ok = vkCreateImage(m_device, &imgInfo, nullptr, &envImage) == VK_SUCCESS &&
                      envImage != VK_NULL_HANDLE;
            if (!ok) OHAO_LOG(Error, gpu, "RT: env map: vkCreateImage failed for {}", m_envMapPath);
            if (ok) {
                VkMemoryRequirements emr{};
                vkGetImageMemoryRequirements(m_device, envImage, &emr);
                VkMemoryAllocateInfo eai{};
                eai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                eai.allocationSize = emr.size;
                eai.memoryTypeIndex = findMemoryType(m_physicalDevice, emr.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                ok = vkAllocateMemory(m_device, &eai, nullptr, &envMem) == VK_SUCCESS && envMem != VK_NULL_HANDLE;
                if (!ok) {
                    OHAO_LOG(Error, gpu, "RT: env map: OOM allocating {} bytes for {}",
                             static_cast<uint64_t>(emr.size), m_envMapPath);
                    vkDestroyImage(m_device, envImage, nullptr);
                }
            }
            if (ok) {
                vkBindImageMemory(m_device, envImage, envMem, 0);
                m_envMapImage = envImage;    // owned; freed by destroyEnvMapImage()
                m_envMapMemory = envMem;

                // The path tracer's 2D CDF (render/rt/env_cdf.hpp), uploaded with the pixels through
                // one staging buffer into device-local memory: its draws are random reads, which host
                // memory (over PCIe) made most of a realtime frame.
                EnvCdf envCdf;
                envCdf.build(hdrPixels, ew, eh);
                destroyEnvSamplingBuffer();   // the queue is idle: no env map yet, or waited above

                // Upload via staging buffer
                const VkDeviceSize pixelSize = VkDeviceSize(ew) * eh * 4 * sizeof(float);
                const VkDeviceSize cdfSize = VkDeviceSize(envCdf.data().size()) * sizeof(float);
                {
                    VkBufferCreateInfo bci{};
                    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bci.size = cdfSize;
                    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    vkCreateBuffer(m_device, &bci, nullptr, &m_envCdfBuffer);
                    VkMemoryRequirements mr;
                    vkGetBufferMemoryRequirements(m_device, m_envCdfBuffer, &mr);
                    VkMemoryAllocateInfo ai{};
                    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    ai.allocationSize = mr.size;
                    ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    vkAllocateMemory(m_device, &ai, nullptr, &m_envCdfMemory);
                    vkBindBufferMemory(m_device, m_envCdfBuffer, m_envCdfMemory, 0);
                }
                VkBuffer stagingBuf = VK_NULL_HANDLE;
                VkDeviceMemory stagingMem = VK_NULL_HANDLE;
                VkBufferCreateInfo stagingBci{};
                stagingBci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                stagingBci.size = pixelSize + cdfSize;
                stagingBci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                vkCreateBuffer(m_device, &stagingBci, nullptr, &stagingBuf);
                VkMemoryRequirements smr;
                vkGetBufferMemoryRequirements(m_device, stagingBuf, &smr);
                VkMemoryAllocateInfo sai{};
                sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                sai.allocationSize = smr.size;
                sai.memoryTypeIndex = findMemoryType(m_physicalDevice, smr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(m_device, &sai, nullptr, &stagingMem);
                vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);
                void* sm = nullptr;
                vkMapMemory(m_device, stagingMem, 0, pixelSize + cdfSize, 0, &sm);
                std::memcpy(sm, hdrPixels, static_cast<size_t>(pixelSize));
                std::memcpy(static_cast<char*>(sm) + pixelSize, envCdf.data().data(), static_cast<size_t>(cdfSize));
                vkUnmapMemory(m_device, stagingMem);

                VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
                VkCommandBufferAllocateInfo cmdInfo{};
                cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cmdInfo.commandPool = m_commandPool;
                cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cmdInfo.commandBufferCount = 1;
                vkAllocateCommandBuffers(m_device, &cmdInfo, &uploadCmd);
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(uploadCmd, &beginInfo);

                VkImageMemoryBarrier toDst{};
                toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.image = envImage;
                toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {static_cast<uint32_t>(ew), static_cast<uint32_t>(eh), 1};
                vkCmdCopyBufferToImage(uploadCmd, stagingBuf, envImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                const VkBufferCopy cdfCopy{pixelSize, 0, cdfSize};
                vkCmdCopyBuffer(uploadCmd, stagingBuf, m_envCdfBuffer, 1, &cdfCopy);
                VkBufferMemoryBarrier cdfReady{};
                cdfReady.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                cdfReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                cdfReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                cdfReady.srcQueueFamilyIndex = cdfReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                cdfReady.buffer = m_envCdfBuffer;
                cdfReady.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                     0, nullptr, 1, &cdfReady, 0, nullptr);

                // Sampled by the path tracer (RT stages) and the deferred passes.
                VkImageMemoryBarrier toShader{};
                toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.srcQueueFamilyIndex = toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toShader.image = envImage;
                toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);

                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = envImage;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageView envView = VK_NULL_HANDLE;
                vkCreateImageView(m_device, &viewInfo, nullptr, &envView);
                m_envMapImageView = envView;

                // The deferred frame's IBL from the same image, in the same submission (the
                // queue was idled above, so an earlier prefilter's images are free to replace).
                if (!m_envPrefilter) {
                    m_envPrefilter = std::make_unique<render::EnvPrefilter>();
                    if (!m_envPrefilter->initialize(m_device, m_physicalDevice)) {
                        OHAO_LOG(Error, gpu, "env map: the IBL prefilter could not start; deferred IBL is off");
                        m_envPrefilter.reset();
                    }
                }
                if (m_envPrefilter && !m_envPrefilter->build(uploadCmd, envView, static_cast<uint32_t>(ew),
                                                              static_cast<uint32_t>(eh)))
                    OHAO_LOG(Error, gpu, "env map: prefiltering {} failed; deferred IBL is off", m_envMapPath);

                vkEndCommandBuffer(uploadCmd);
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &uploadCmd;
                gpu::submitAndWait(m_device, m_graphicsQueue, submitInfo, "environment upload");
                vkDestroyBuffer(m_device, stagingBuf, nullptr);
                vkFreeMemory(m_device, stagingMem, nullptr);
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);

                OHAO_LOG(Debug, gpu, "RT: env CDF: {}x{}", ew, eh);

                // Into the one bindless table. The manager now owns the view.
                const BindlessTextureHandle h =
                    m_textureManager->registerExternalTexture(envView, "ohao_env_map", BindlessTextureType::Custom);
                m_envMapTexIdx = h.valid() ? h.index : 0xFFFFFFFFu;
                m_envMapWidth = static_cast<uint32_t>(ew);
                m_envMapHeight = static_cast<uint32_t>(eh);
                m_envMapLoadedKey = key;
                OHAO_LOG(Info, gpu, "RT: Environment map loaded: {}x{} (bindless idx={})", ew, eh,
                         m_envMapTexIdx);
            }
            if (skyPixels.empty()) stbi_image_free(hdrPixels);
        }
    }

    // Binding 17 stays valid without an env map (envWidth 0 tells the shader to skip it): one
    // float, never read.
    if (!m_envCdfBuffer) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sizeof(float);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &m_envCdfBuffer);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, m_envCdfBuffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_envCdfMemory);
        vkBindBufferMemory(m_device, m_envCdfBuffer, m_envCdfMemory, 0);
    }

    if (m_renderScene) {
        m_renderScene->setEnvMapTextureIndex(envMapLoaded() ? m_envMapTexIdx : render::kNoTexture);
        m_renderScene->setEnvIntensity(m_envIntensityScale);
    }
}

void VulkanRenderer::bindRTTextures(IRTRendererProfile& renderer) {
    if (!m_textureManager) return;
    collectRTTextures();
    renderer.setBindlessTextures(m_rtTextureViews, m_rtTextureSamplers);
}

void VulkanRenderer::collectRTTextures() {
    if (!m_textureManager) return;
    // Every live slot of the one bindless table, index = bindless index. Holes (freed
    // slots) get the default white texture: the path tracer writes the array densely
    // up to its binding capacity (the table's capacity, set in ensureRTRenderer).
    const uint32_t count = m_textureManager->usedSlotEnd();
    const VkImageView fallback = m_textureManager->getImageView(m_textureManager->getDefaultWhiteTexture());
    m_rtTextureViews.resize(count);
    m_rtTextureSamplers.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const VkImageView view = m_textureManager->getImageView(BindlessTextureHandle{i});
        m_rtTextureViews[i] = view != VK_NULL_HANDLE ? view : fallback;
        m_rtTextureSamplers[i] = (envMapLoaded() && i == m_envMapTexIdx && m_envSampler != VK_NULL_HANDLE)
                                     ? m_envSampler
                                     : m_textureManager->getDefaultSampler();
    }
}

void VulkanRenderer::destroyEnvSamplingBuffer() {
    if (m_envCdfBuffer) { vkDestroyBuffer(m_device, m_envCdfBuffer, nullptr); m_envCdfBuffer = VK_NULL_HANDLE; }
    if (m_envCdfMemory) { vkFreeMemory(m_device, m_envCdfMemory, nullptr); m_envCdfMemory = VK_NULL_HANDLE; }
}

void VulkanRenderer::destroyEnvMapImage() {
    // The bindless table owns the view once registered: unloading it destroys the view.
    if (m_envMapTexIdx != 0xFFFFFFFFu && m_textureManager && m_textureManager->isInitialized()) {
        m_textureManager->unloadTexture(BindlessTextureHandle{m_envMapTexIdx});
        m_envMapImageView = VK_NULL_HANDLE;
    }
    m_envMapTexIdx = 0xFFFFFFFFu;
    if (m_envMapImageView) { vkDestroyImageView(m_device, m_envMapImageView, nullptr); m_envMapImageView = VK_NULL_HANDLE; }
    if (m_envMapImage) { vkDestroyImage(m_device, m_envMapImage, nullptr); m_envMapImage = VK_NULL_HANDLE; }
    if (m_envMapMemory) { vkFreeMemory(m_device, m_envMapMemory, nullptr); m_envMapMemory = VK_NULL_HANDLE; }
    m_envMapWidth = 0;
    m_envMapHeight = 0;
    m_envMapLoadedKey.clear();
}

} // namespace ohao
