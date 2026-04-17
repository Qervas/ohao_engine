#include "renderer_impl.hpp"
#include <array>
#include <deque>
#include "scene/scene.hpp"
#include "scene/component/light_component.hpp"
#include "render/rt/gpu_light.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "stb_image.h"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/asset/model.hpp"
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include "render/deferred/deferred_renderer.hpp"

namespace ohao {


// Light buffer, env map, deferred texture bridge
// Extracted from buildAccelerationStructures()



void VulkanRenderer::uploadDeferredTextures() {
    // Load model textures into BindlessTextureManager for deferred pipeline
    if (m_textureManager && m_scene) {
        int deferredTexCount = 0;
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto mc = actor->getComponent<MeshComponent>();
            if (!mc || !mc->getModel()) continue;
            auto model = mc->getModel();
            auto matComp = actor->getComponent<MaterialComponent>();
            if (!matComp) continue;

            // Load albedo textures
            for (size_t mi = 0; mi < model->materialTextureIndex.size(); mi++) {
                int texIdx = model->materialTextureIndex[mi];
                if (texIdx < 0 || texIdx >= static_cast<int>(model->albedoTextures.size())) continue;
                const auto& td = model->albedoTextures[texIdx];
                if (td.pixels.empty() || td.width <= 0 || td.height <= 0) continue;

                std::string texName = actor->getName() + "_albedo_" + std::to_string(mi);
                auto handle = m_textureManager->loadTextureFromMemory(
                    td.pixels.data(), td.width, td.height, VK_FORMAT_R8G8B8A8_SRGB,
                    BindlessTextureType::Albedo);
                if (handle.valid()) {
                    m_textureManager->registerName(handle, texName);
                    matComp->getMaterial().useAlbedoTexture = true;
                    matComp->getMaterial().albedoTexture = texName;
                    deferredTexCount++;
                }
            }

            // Load normal textures
            for (size_t mi = 0; mi < model->materialNormalTexIndex.size(); mi++) {
                int nTexIdx = model->materialNormalTexIndex[mi];
                if (nTexIdx < 0 || nTexIdx >= static_cast<int>(model->normalTextures.size())) continue;
                const auto& ntd = model->normalTextures[nTexIdx];
                if (ntd.pixels.empty()) continue;

                std::string texName = actor->getName() + "_normal_" + std::to_string(mi);
                auto handle = m_textureManager->loadTextureFromMemory(
                    ntd.pixels.data(), ntd.width, ntd.height, VK_FORMAT_R8G8B8A8_UNORM,
                    BindlessTextureType::Normal);
                if (handle.valid()) {
                    m_textureManager->registerName(handle, texName);
                    matComp->getMaterial().useNormalTexture = true;
                    matComp->getMaterial().normalTexture = texName;
                }
            }

            // Set roughness/metallic from model data
            if (!model->materialColors.empty()) {
                float roughness = model->materialColors[0].w;
                float metallic = !model->materialMetallic.empty() ? model->materialMetallic[0] : 0.0f;
                matComp->getMaterial().roughness = roughness;
                matComp->getMaterial().metallic = metallic;
            }

            // Load roughness/metallic texture
            for (size_t mi = 0; mi < model->materialRoughMetalTexIndex.size(); mi++) {
                int rmIdx = model->materialRoughMetalTexIndex[mi];
                if (rmIdx < 0 || rmIdx >= static_cast<int>(model->roughMetalTextures.size())) continue;
                const auto& rmtd = model->roughMetalTextures[rmIdx];
                if (rmtd.pixels.empty()) continue;

                // REPACK: Assimp gives raw GLTF (R=unused/AO, G=Roughness, B=Metallic)
                // We must ensure the Bindless texture matches our shader layout (R=AO, G=Roughness, B=Metallic)
                // If it's already 4 channels, we'll verify/swizzle it to be safe.
                std::vector<uint8_t> repacked;
                repacked.resize(rmtd.width * rmtd.height * 4);
                for (int p = 0; p < rmtd.width * rmtd.height; p++) {
                    // Assimp usually provides 4 channels if we requested it, but the order is raw
                    uint8_t r = rmtd.pixels[p*4+0]; // AO (or 255)
                    uint8_t g = rmtd.pixels[p*4+1]; // Roughness
                    uint8_t b = rmtd.pixels[p*4+2]; // Metallic
                    repacked[p*4+0] = r;   // R = AO
                    repacked[p*4+1] = g;   // G = Roughness
                    repacked[p*4+2] = b;   // B = Metallic
                    repacked[p*4+3] = 255;
                }

                std::string texName = actor->getName() + "_roughmetal_" + std::to_string(mi);
                auto handle = m_textureManager->loadTextureFromMemory(
                    repacked.data(), rmtd.width, rmtd.height, VK_FORMAT_R8G8B8A8_UNORM,
                    BindlessTextureType::Roughness);
                if (handle.valid()) {
                    m_textureManager->registerName(handle, texName);
                    matComp->getMaterial().useRoughnessTexture = true;
                    matComp->getMaterial().roughnessTexture = texName;
                    deferredTexCount++;
                }
            }

            // Load emissive textures
            for (size_t mi = 0; mi < model->materialEmissiveTexIndex.size(); mi++) {
                int emIdx = model->materialEmissiveTexIndex[mi];
                if (emIdx < 0 || emIdx >= static_cast<int>(model->emissiveTextures.size())) continue;
                const auto& etd = model->emissiveTextures[emIdx];
                if (etd.pixels.empty()) continue;

                std::string texName = actor->getName() + "_emissive_" + std::to_string(mi);
                auto handle = m_textureManager->loadTextureFromMemory(
                    etd.pixels.data(), etd.width, etd.height, VK_FORMAT_R8G8B8A8_SRGB,
                    BindlessTextureType::Emissive);
                if (handle.valid()) {
                    m_textureManager->registerName(handle, texName);
                    matComp->getMaterial().useEmissiveTexture = true;
                    matComp->getMaterial().emissiveTexture = texName;
                    deferredTexCount++;
                }
            }
        }
        if (deferredTexCount > 0) {
            m_textureManager->updateDescriptorSet();
            std::cout << "[Deferred] Loaded " << deferredTexCount << " textures into BindlessTextureManager" << std::endl;
        }
    }

}

void VulkanRenderer::uploadLightBuffer() {
    // Build light buffer from scene LightComponents
    {
        std::vector<GPULight> gpuLights;
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto lc = actor->getComponent<LightComponent>();
            if (!lc) continue;

            GPULight gl{};
            auto pos = actor->getTransform()->getPosition();
            gl.positionAndType = glm::vec4(pos, static_cast<float>(lc->getLightType()));
            gl.colorAndIntensity = glm::vec4(lc->getColor(), lc->getIntensity());
            float dirParam = lc->getRadius();
            if (lc->getLightType() == LightType::Spot) {
                dirParam = lc->getInnerConeAngle();
            }
            gl.dirAndParam = glm::vec4(lc->getDirection(), dirParam);

            if (lc->getLightType() == LightType::AreaRect) {
                glm::vec3 e1 = lc->getEdge1(), e2 = lc->getEdge2();
                float area = glm::length(glm::cross(e1, e2));
                gl.extra = glm::vec4(e1, 0.0f);
                gl.extra2 = glm::vec4(e2, area);
            } else {
                gl.extra = glm::vec4(0.0f, 0.0f, 0.0f, lc->getOuterConeAngle());
                gl.extra2 = glm::vec4(0.0f);
            }
            gpuLights.push_back(gl);
        }

        // Auto-generate lights from emissive materials
        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto mc = actor->getComponent<MeshComponent>();
            if (!mc || !mc->getModel()) continue;
            auto model = mc->getModel();

            // Check if any material has an emissive texture
            for (size_t mi = 0; mi < model->materialEmissiveTexIndex.size(); mi++) {
                if (model->materialEmissiveTexIndex[mi] < 0) continue;

                // Compute mesh center and radius for this actor
                glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
                for (const auto& v : model->vertices) {
                    bmin = glm::min(bmin, v.position);
                    bmax = glm::max(bmax, v.position);
                }
                glm::mat4 worldMat = actor->getTransform()->getWorldMatrix();
                glm::vec3 center = glm::vec3(worldMat * glm::vec4((bmin + bmax) * 0.5f, 1.0f));
                float radius = glm::length(bmax - bmin) * 0.3f;

                // Compute emissive color: use sum of bright pixels (not average of all)
                glm::vec3 emColor(1.0f);
                float totalPower = 0.0f;
                int eTexIdx = model->materialEmissiveTexIndex[mi];
                if (eTexIdx >= 0 && eTexIdx < static_cast<int>(model->emissiveTextures.size())) {
                    const auto& etd = model->emissiveTextures[eTexIdx];
                    double r = 0, g = 0, b = 0;
                    int brightPixels = 0;
                    for (int p = 0; p < etd.width * etd.height; p++) {
                        float pr = etd.pixels[p*4+0] / 255.0f;
                        float pg = etd.pixels[p*4+1] / 255.0f;
                        float pb = etd.pixels[p*4+2] / 255.0f;
                        float lum = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;
                        if (lum > 0.05f) {  // only count bright pixels
                            r += pr; g += pg; b += pb;
                            brightPixels++;
                            totalPower += lum;
                        }
                    }
                    if (brightPixels > 0) {
                        emColor = glm::vec3(r / brightPixels, g / brightPixels, b / brightPixels);
                    }
                }

                if (totalPower > 0.1f) {
                    float intensity = std::min(totalPower * 0.1f, 20.0f);  // scale power to reasonable intensity
                    GPULight gl{};
                    gl.positionAndType = glm::vec4(center, 0.0f);  // sphere type
                    gl.colorAndIntensity = glm::vec4(emColor, intensity);
                    gl.dirAndParam = glm::vec4(0, -1, 0, radius);
                    gl.extra = glm::vec4(0);
                    gl.extra2 = glm::vec4(0);
                    gpuLights.push_back(gl);
                    std::cout << "[RT] Emissive mesh light: " << actor->getName()
                              << " color=(" << emColor.r << "," << emColor.g << "," << emColor.b
                              << ") intensity=" << intensity << std::endl;
                }
                break;  // one light per actor
            }
        }

        // Destroy old buffer
        if (m_rtLightBuffer) { vkDestroyBuffer(m_device, m_rtLightBuffer, nullptr); m_rtLightBuffer = VK_NULL_HANDLE; }
        if (m_rtLightMemory) { vkFreeMemory(m_device, m_rtLightMemory, nullptr); m_rtLightMemory = VK_NULL_HANDLE; }
        m_rtLightCount = 0;

        if (!gpuLights.empty()) {
            // Buffer layout: uint32 lightCount + 12 bytes padding + GPULight[] lights
            // GLSL std430 aligns vec4 struct members to 16 bytes
            VkDeviceSize lightDataOffset = 16;  // align GPULight array to 16 bytes
            VkDeviceSize bufSize = lightDataOffset + gpuLights.size() * sizeof(GPULight);
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = bufSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            vkCreateBuffer(m_device, &bci, nullptr, &m_rtLightBuffer);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(m_device, m_rtLightBuffer, &mr);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_device, &ai, nullptr, &m_rtLightMemory);
            vkBindBufferMemory(m_device, m_rtLightBuffer, m_rtLightMemory, 0);

            void* mapped;
            vkMapMemory(m_device, m_rtLightMemory, 0, bufSize, 0, &mapped);
            memset(mapped, 0xFF, 16);  // init header to 0xFFFFFFFF (no env map by default)
            uint32_t count = static_cast<uint32_t>(gpuLights.size());
            m_rtLightCount = count;
            memcpy(mapped, &count, sizeof(uint32_t));
            // envMapTexIdx at offset 4 — set by env map loader, default 0xFFFFFFFF (none)
            memcpy(static_cast<uint8_t*>(mapped) + lightDataOffset, gpuLights.data(), gpuLights.size() * sizeof(GPULight));
            vkUnmapMemory(m_device, m_rtLightMemory);

            // Load environment map if set
            if (!m_envMapPath.empty() && m_rtLightMemory) {
                int ew, eh, ec;
                float* hdrPixels = stbi_loadf(m_envMapPath.c_str(), &ew, &eh, &ec, 4);
                if (hdrPixels) {
                    // Add HDR as a bindless texture
                    // Create VkImage (RGBA16F for HDR)
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

                    VkImage envImage;
                    vkCreateImage(m_device, &imgInfo, nullptr, &envImage);
                    VkMemoryRequirements emr;
                    vkGetBufferMemoryRequirements(m_device, m_rtLightBuffer, &emr);  // reuse for sizing
                    vkGetImageMemoryRequirements(m_device, envImage, &emr);
                    VkMemoryAllocateInfo eai{};
                    eai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    eai.allocationSize = emr.size;
                    eai.memoryTypeIndex = findMemoryType(m_physicalDevice, emr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    VkDeviceMemory envMem;
                    vkAllocateMemory(m_device, &eai, nullptr, &envMem);
                    vkBindImageMemory(m_device, envImage, envMem, 0);

                    // Upload via staging buffer
                    VkDeviceSize pixelSize = ew * eh * 4 * sizeof(float);
                    VkBuffer stagingBuf;
                    VkDeviceMemory stagingMem;
                    VkBufferCreateInfo stagingBci{};
                    stagingBci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    stagingBci.size = pixelSize;
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
                    void* sm;
                    vkMapMemory(m_device, stagingMem, 0, pixelSize, 0, &sm);
                    memcpy(sm, hdrPixels, pixelSize);
                    vkUnmapMemory(m_device, stagingMem);
                    stbi_image_free(hdrPixels);

                    // Copy to image
                    VkCommandBuffer uploadCmd;
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

                    // Transition to TRANSFER_DST
                    VkImageMemoryBarrier toDst{};
                    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toDst.image = envImage;
                    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {static_cast<uint32_t>(ew), static_cast<uint32_t>(eh), 1};
                    vkCmdCopyBufferToImage(uploadCmd, stagingBuf, envImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                    // Transition to SHADER_READ
                    VkImageMemoryBarrier toShader{};
                    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    toShader.image = envImage;
                    toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &toShader);

                    vkEndCommandBuffer(uploadCmd);
                    VkSubmitInfo submitInfo{};
                    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    submitInfo.commandBufferCount = 1;
                    submitInfo.pCommandBuffers = &uploadCmd;
                    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                    vkQueueWaitIdle(m_graphicsQueue);
                    vkDestroyBuffer(m_device, stagingBuf, nullptr);
                    vkFreeMemory(m_device, stagingMem, nullptr);
                    vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);

                    // Create image view
                    VkImageViewCreateInfo viewInfo{};
                    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    viewInfo.image = envImage;
                    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkImageView envView;
                    vkCreateImageView(m_device, &viewInfo, nullptr, &envView);
                    m_envMapImageView = envView;  // store for deferred pipeline

                    // Add to bindless textures
                    auto views = m_pathTracer->getBindlessImageViews();
                    auto samplers = m_pathTracer->getBindlessSamplers();
                    uint32_t envTexIdx = static_cast<uint32_t>(views.size());
                    views.push_back(envView);
                    samplers.push_back(m_rtTextureSampler);
                    m_pathTracer->setBindlessTextures(views, samplers);

                    // Write env map index to light buffer header (offset 4)
                    void* lm;
                    vkMapMemory(m_device, m_rtLightMemory, 0, 16, 0, &lm);
                    memcpy(static_cast<uint8_t*>(lm) + 4, &envTexIdx, sizeof(uint32_t));
                    vkUnmapMemory(m_device, m_rtLightMemory);

                    std::cout << "[RT] Environment map loaded: " << ew << "x" << eh
                              << " (bindless idx=" << envTexIdx << ")" << std::endl;
                }
            }

            if (m_pathTracer) {
                m_pathTracer->setLightBuffer(m_rtLightBuffer, count);
            }

            std::cout << "[RT] Light buffer: " << count << " lights" << std::endl;
        }
    }

}

} // namespace ohao
