#include "vulkan_renderer_impl.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include "render/graph/pass.hpp"
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
    // unloads what was there.
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
            // Replacing an earlier environment: its image and its CDF are retired, for the frames in flight that
            // still read them.
            destroyEnvMapImage();
            m_allocator.retire(m_envCdf);

            // The path tracer's 2D CDF (render/rt/env_cdf.hpp), uploaded with the pixels through
            // one staging buffer into device-local memory: its draws are random reads, which host
            // memory (over PCIe) made most of a realtime frame.
            EnvCdf envCdf;
            envCdf.build(hdrPixels, ew, eh);
            const VkDeviceSize pixelSize = VkDeviceSize(ew) * eh * 4 * sizeof(float);
            const VkDeviceSize cdfSize = VkDeviceSize(envCdf.data().size()) * sizeof(float);

            m_envMap = m_allocator.createImage(
                {"env.map", VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(ew), static_cast<uint32_t>(eh),
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT});
            m_envCdf = m_allocator.createBuffer(
                cdfSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, AllocationUsage::GpuOnly);
            GpuBuffer staging = m_allocator.createBuffer(pixelSize + cdfSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                         AllocationUsage::CpuToGpu, true);
            if (!m_envMap || !m_envCdf.isValid() || !staging.getMappedData()) {
                OHAO_LOG(Error, gpu, "RT: env map: no memory for {} ({}x{})", m_envMapPath, ew, eh);
                m_allocator.retire(m_envMap);
                m_allocator.destroyBuffer(m_envCdf);
                m_envCdf = GpuBuffer{};
                m_allocator.destroyBuffer(staging);
            } else {
                auto* const sm = static_cast<char*>(staging.getMappedData());
                std::memcpy(sm, hdrPixels, static_cast<size_t>(pixelSize));
                std::memcpy(sm + pixelSize, envCdf.data().data(), static_cast<size_t>(cdfSize));
                m_allocator.flushBuffer(staging);

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
                const render::Frame frame{uploadCmd, nullptr, 0};
                {
                    render::Pass pass(frame, nullptr, render::Pass::Transfer);
                    pass.read(staging, render::BufferUse::CopySrc).copyDst(m_envMap);
                    pass.write(m_envCdf, render::BufferUse::CopyDst);
                    pass.begin();
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {static_cast<uint32_t>(ew), static_cast<uint32_t>(eh), 1};
                    vkCmdCopyBufferToImage(uploadCmd, staging.buffer, m_envMap.image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    const VkBufferCopy cdfCopy{pixelSize, 0, cdfSize};
                    vkCmdCopyBuffer(uploadCmd, staging.buffer, m_envCdf.buffer, 1, &cdfCopy);
                }

                // The deferred frame's IBL from the same image, in the same submission.
                if (!m_envPrefilter) {
                    m_envPrefilter = std::make_unique<render::EnvPrefilter>();
                    if (!m_envPrefilter->initialize(m_device, m_allocator)) {
                        OHAO_LOG(Error, gpu, "env map: the IBL prefilter could not start; deferred IBL is off");
                        m_envPrefilter.reset();
                    }
                }
                if (m_envPrefilter && !m_envPrefilter->build(frame, m_envMap))
                    OHAO_LOG(Error, gpu, "env map: prefiltering {} failed; deferred IBL is off", m_envMapPath);

                vkEndCommandBuffer(uploadCmd);
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &uploadCmd;
                gpu::submitAndWait(m_device, m_graphicsQueue, submitInfo, "environment upload");
                m_allocator.destroyBuffer(staging);
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);

                OHAO_LOG(Debug, gpu, "RT: env CDF: {}x{}", ew, eh);

                // Into the one bindless table; the view stays the image's.
                const BindlessTextureHandle h =
                    m_textureManager->registerExternalTexture(m_envMap.view, "ohao_env_map", BindlessTextureType::Custom);
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
    if (!m_envCdf.isValid())
        m_envCdf = m_allocator.createBuffer(sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);

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

void VulkanRenderer::destroyEnvMapImage() {
    // Out of the bindless table, then retired: a frame in flight may still sample it.
    if (m_envMapTexIdx != 0xFFFFFFFFu && m_textureManager && m_textureManager->isInitialized())
        m_textureManager->unloadTexture(BindlessTextureHandle{m_envMapTexIdx});
    m_envMapTexIdx = 0xFFFFFFFFu;
    m_allocator.retire(m_envMap);
    m_envMapWidth = 0;
    m_envMapHeight = 0;
    m_envMapLoadedKey.clear();
}

} // namespace ohao
