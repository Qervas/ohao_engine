#include "offscreen_renderer_impl.hpp"
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


// RT buffer creation, material/texture upload, BLAS/TLAS
// Extracted from buildAccelerationStructures()



void OffscreenRenderer::createRTVertexIndexBuffers() {
    // Destroy old RT buffers
    if (m_rtVertexBuffer) { vkDestroyBuffer(m_device, m_rtVertexBuffer, nullptr); m_rtVertexBuffer = VK_NULL_HANDLE; }
    if (m_rtVertexMemory) { vkFreeMemory(m_device, m_rtVertexMemory, nullptr); m_rtVertexMemory = VK_NULL_HANDLE; }
    if (m_rtIndexBuffer) { vkDestroyBuffer(m_device, m_rtIndexBuffer, nullptr); m_rtIndexBuffer = VK_NULL_HANDLE; }
    if (m_rtIndexMemory) { vkFreeMemory(m_device, m_rtIndexMemory, nullptr); m_rtIndexMemory = VK_NULL_HANDLE; }

    // Create device-local RT buffers with proper flags
    auto createRTBuffer = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem,
                              VkBuffer srcBuf) -> bool {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buf) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, buf, &memReqs);

        VkMemoryAllocateFlagsInfo allocFlags{};
        allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &allocFlags;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buf, mem, 0);

        // Copy data from raster buffer to RT buffer
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

        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, srcBuf, buf, 1, &copyRegion);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return true;
    };

    VkDeviceSize vertexSize = m_vertexCount * sizeof(Vertex);
    VkDeviceSize indexSize = m_indexCount * sizeof(uint32_t);

    if (!createRTBuffer(vertexSize, m_rtVertexBuffer, m_rtVertexMemory, m_vertexBuffer)) {
        std::cerr << "[RT] Failed to create RT vertex buffer" << std::endl;
        return;
    }
    if (!createRTBuffer(indexSize, m_rtIndexBuffer, m_rtIndexMemory, m_indexBuffer)) {
        std::cerr << "[RT] Failed to create RT index buffer" << std::endl;
        return;
    }

}

void OffscreenRenderer::createRTNormalUVBuffers() {
    VkDeviceSize vertexSize = m_vertexCount * sizeof(Vertex);
    // Create normal buffer — extract normals from host-visible vertex buffer
    {
        if (m_rtNormalBuffer) { vkDestroyBuffer(m_device, m_rtNormalBuffer, nullptr); m_rtNormalBuffer = VK_NULL_HANDLE; }
        if (m_rtNormalMemory) { vkFreeMemory(m_device, m_rtNormalMemory, nullptr); m_rtNormalMemory = VK_NULL_HANDLE; }

        // Read vertex data from host-visible buffer to extract normals
        VkDeviceSize normalBufSize = m_vertexCount * sizeof(glm::vec4);
        std::vector<glm::vec4> normals(m_vertexCount);

        void* vertMapped;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexSize, 0, &vertMapped);
        const Vertex* verts = static_cast<const Vertex*>(vertMapped);
        for (uint32_t i = 0; i < m_vertexCount; i++) {
            normals[i] = glm::vec4(verts[i].normal, 0.0f);
        }
        vkUnmapMemory(m_device, m_vertexBufferMemory);

        // Create device-local normal buffer
        VkBufferCreateInfo nbInfo{};
        nbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        nbInfo.size = normalBufSize;
        nbInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(m_device, &nbInfo, nullptr, &m_rtNormalBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, m_rtNormalBuffer, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &allocInfo, nullptr, &m_rtNormalMemory);
        vkBindBufferMemory(m_device, m_rtNormalBuffer, m_rtNormalMemory, 0);

        void* normalMapped;
        vkMapMemory(m_device, m_rtNormalMemory, 0, normalBufSize, 0, &normalMapped);
        memcpy(normalMapped, normals.data(), normalBufSize);
        vkUnmapMemory(m_device, m_rtNormalMemory);

        std::cout << "[RT] Normal buffer created: " << m_vertexCount << " normals" << std::endl;
    }

    // Create UV buffer — extract texcoords from vertex data
    {
        if (m_rtUVBuffer) { vkDestroyBuffer(m_device, m_rtUVBuffer, nullptr); m_rtUVBuffer = VK_NULL_HANDLE; }
        if (m_rtUVMemory) { vkFreeMemory(m_device, m_rtUVMemory, nullptr); m_rtUVMemory = VK_NULL_HANDLE; }

        VkDeviceSize uvBufSize = m_vertexCount * sizeof(glm::vec2);
        std::vector<glm::vec2> uvs(m_vertexCount);
        void* vertMapped;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexSize, 0, &vertMapped);
        const Vertex* verts = static_cast<const Vertex*>(vertMapped);
        for (uint32_t i = 0; i < m_vertexCount; i++) {
            uvs[i] = verts[i].texCoord;
        }
        vkUnmapMemory(m_device, m_vertexBufferMemory);

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = uvBufSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &m_rtUVBuffer);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, m_rtUVBuffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_rtUVMemory);
        vkBindBufferMemory(m_device, m_rtUVBuffer, m_rtUVMemory, 0);
        void* mapped;
        vkMapMemory(m_device, m_rtUVMemory, 0, uvBufSize, 0, &mapped);
        memcpy(mapped, uvs.data(), uvBufSize);
        vkUnmapMemory(m_device, m_rtUVMemory);
        std::cout << "[RT] UV buffer created: " << m_vertexCount << " UVs" << std::endl;
    }

}

void OffscreenRenderer::uploadRTMaterialBuffers() {
    // Create material-ID buffer — per-triangle material index from loaded model
    // And per-material color buffer
    {
        if (m_rtMatIDBuffer) { vkDestroyBuffer(m_device, m_rtMatIDBuffer, nullptr); m_rtMatIDBuffer = VK_NULL_HANDLE; }
        if (m_rtMatIDMemory) { vkFreeMemory(m_device, m_rtMatIDMemory, nullptr); m_rtMatIDMemory = VK_NULL_HANDLE; }
        if (m_rtMatColorBuffer) { vkDestroyBuffer(m_device, m_rtMatColorBuffer, nullptr); m_rtMatColorBuffer = VK_NULL_HANDLE; }
        if (m_rtMatColorMemory) { vkFreeMemory(m_device, m_rtMatColorMemory, nullptr); m_rtMatColorMemory = VK_NULL_HANDLE; }

        // Collect material IDs and colors from all actors' models
        std::vector<uint32_t> allMatIDs;
        std::vector<glm::vec4> allMatColors;

        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto mc = actor->getComponent<MeshComponent>();
            if (!mc || !mc->getModel()) continue;
            auto model = mc->getModel();

            // Append per-triangle material IDs (offset by current material count)
            // allMatColors has 2 vec4s per material, so divide by 2 for material count
            uint32_t colorOffset = static_cast<uint32_t>(allMatColors.size() / 3);
            if (!model->materialPerTriangle.empty()) {
                for (uint32_t mid : model->materialPerTriangle) {
                    allMatIDs.push_back(mid + colorOffset);
                }
            } else {
                // No per-triangle data — all triangles use material 0
                size_t numTris = model->indices.size() / 3;
                for (size_t t = 0; t < numTris; t++) {
                    allMatIDs.push_back(colorOffset);
                }
            }

            // Append material colors — 2 vec4s per material:
            //   [matID*2+0] = (baseColor.rgb, diffuseTexIdx as uint bits)
            //   [matID*2+1] = (roughness, metallic, normalTexIdx, emissiveTexIdx)
            // Texture indices encoded as uint bits via memcpy. 0xFFFFFFFF = no texture.
            // Encode uint as float bits (0xFFFFFFFF = no texture)
            auto packNoTex = []() -> float {
                uint32_t noTex = 0xFFFFFFFFu;
                float f; memcpy(&f, &noTex, sizeof(float)); return f;
            };
            float noTexF = packNoTex();

            // 3 vec4s per material:
            //   [matID*3+0] = (baseColor.rgb, diffuseTexIdx)
            //   [matID*3+1] = (roughness, metallic, normalTexIdx, emissiveTexIdx)
            //   [matID*3+2] = (roughMetalTexIdx, unused, unused, unused)
            if (!model->materialColors.empty()) {
                for (size_t mi = 0; mi < model->materialColors.size(); mi++) {
                    const auto& mc2 = model->materialColors[mi];
                    float metallic = (mi < model->materialMetallic.size()) ? model->materialMetallic[mi] : 0.0f;
                    allMatColors.push_back(glm::vec4(mc2.x, mc2.y, mc2.z, noTexF));
                    allMatColors.push_back(glm::vec4(mc2.w, metallic, noTexF, noTexF));
                    allMatColors.push_back(glm::vec4(noTexF, 0.0f, 0.0f, 0.0f));  // roughMetalTexIdx
                }
            } else {
                auto matComp = actor->getComponent<MaterialComponent>();
                glm::vec3 col(0.8f);
                float rough = 0.5f;
                float metal = 0.0f;
                if (matComp) {
                    col = matComp->getMaterial().baseColor;
                    rough = matComp->getMaterial().roughness;
                    metal = matComp->getMaterial().metallic;
                }
                allMatColors.push_back(glm::vec4(col, noTexF));
                allMatColors.push_back(glm::vec4(rough, metal, noTexF, noTexF));
                allMatColors.push_back(glm::vec4(noTexF, 0.0f, 0.0f, 0.0f));
            }
        }

        if (allMatIDs.empty()) allMatIDs.push_back(0);
        if (allMatColors.empty()) {
            float noTexF;
            uint32_t noTex = 0xFFFFFFFFu;
            memcpy(&noTexF, &noTex, sizeof(float));
            allMatColors.push_back(glm::vec4(0.8f, 0.8f, 0.8f, noTexF));
            allMatColors.push_back(glm::vec4(0.5f, 0.0f, noTexF, noTexF));
            allMatColors.push_back(glm::vec4(noTexF, 0.0f, 0.0f, 0.0f));
        }

        // Upload material ID buffer
        VkDeviceSize matIDSize = allMatIDs.size() * sizeof(uint32_t);
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = matIDSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &m_rtMatIDBuffer);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, m_rtMatIDBuffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_rtMatIDMemory);
        vkBindBufferMemory(m_device, m_rtMatIDBuffer, m_rtMatIDMemory, 0);
        void* mapped;
        vkMapMemory(m_device, m_rtMatIDMemory, 0, matIDSize, 0, &mapped);
        memcpy(mapped, allMatIDs.data(), matIDSize);
        vkUnmapMemory(m_device, m_rtMatIDMemory);

        // Upload material color buffer
        VkDeviceSize matColorSize = allMatColors.size() * sizeof(glm::vec4);
        bci.size = matColorSize;
        vkCreateBuffer(m_device, &bci, nullptr, &m_rtMatColorBuffer);
        vkGetBufferMemoryRequirements(m_device, m_rtMatColorBuffer, &mr);
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(m_physicalDevice, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &ai, nullptr, &m_rtMatColorMemory);
        vkBindBufferMemory(m_device, m_rtMatColorBuffer, m_rtMatColorMemory, 0);
        vkMapMemory(m_device, m_rtMatColorMemory, 0, matColorSize, 0, &mapped);
        memcpy(mapped, allMatColors.data(), matColorSize);
        vkUnmapMemory(m_device, m_rtMatColorMemory);

        std::cout << "[RT] Material buffers created: " << allMatIDs.size() << " triangles, "
                  << (allMatColors.size() / 3) << " materials (3 vec4s each)" << std::endl;
    }

}

void OffscreenRenderer::uploadRTTextureArray() {
    // Create texture array from all actors' model textures
    {
        // Cleanup previous texture array resources
        if (m_rtTextureSampler) { vkDestroySampler(m_device, m_rtTextureSampler, nullptr); m_rtTextureSampler = VK_NULL_HANDLE; }
        if (m_rtTextureArrayView) { vkDestroyImageView(m_device, m_rtTextureArrayView, nullptr); m_rtTextureArrayView = VK_NULL_HANDLE; }
        if (m_rtTextureArray) { vkDestroyImage(m_device, m_rtTextureArray, nullptr); m_rtTextureArray = VK_NULL_HANDLE; }
        if (m_rtTextureArrayMemory) { vkFreeMemory(m_device, m_rtTextureArrayMemory, nullptr); m_rtTextureArrayMemory = VK_NULL_HANDLE; }
        m_rtTextureCount = 0;

        // Collect all texture data from all actors' models
        struct CollectedTexture {
            const uint8_t* pixels;
            int width, height;
        };
        std::vector<CollectedTexture> allTextures;

        // Build mapping: global material index -> texture array layer
        // We'll update matColors[].a with the texture layer index
        // To do this we need to re-read the matColor buffer and write it back
        std::vector<int> globalMatTexLayer;        // diffuse tex index per material
        std::vector<int> globalNormalTexLayer;    // normal tex index per material
        std::vector<int> globalRoughMetalTexLayer; // roughness+metallic tex index
        std::vector<int> globalEmissiveTexLayer;   // emissive tex index
        uint32_t globalMatOffset = 0;

        // Generate 1x1 solid color textures for materials without real textures
        std::deque<std::array<uint8_t, 4>> solidColorPixels;

        for (const auto& [actorId, actor] : m_scene->getAllActors()) {
            auto mc = actor->getComponent<MeshComponent>();
            if (!mc || !mc->getModel()) continue;
            auto model = mc->getModel();

            size_t numMats = model->materialColors.empty() ? 1 : model->materialColors.size();

            for (size_t matIdx = 0; matIdx < numMats; matIdx++) {
                int texLayer = -1;

                // Check for real texture first
                if (matIdx < model->materialTextureIndex.size()) {
                    int texIdx = model->materialTextureIndex[matIdx];
                    if (texIdx >= 0 && texIdx < static_cast<int>(model->albedoTextures.size())) {
                        const auto& td = model->albedoTextures[texIdx];
                        if (!td.pixels.empty() && td.width > 0 && td.height > 0) {
                            texLayer = static_cast<int>(allTextures.size());
                            CollectedTexture ct;
                            ct.pixels = td.pixels.data();
                            ct.width = td.width;
                            ct.height = td.height;
                            allTextures.push_back(ct);
                        }
                    }
                }

                // No texture — generate 1x1 solid color from material base color
                if (texLayer < 0) {
                    glm::vec3 col(0.8f);
                    if (matIdx < model->materialColors.size()) {
                        col = glm::vec3(model->materialColors[matIdx]);
                    } else {
                        auto matComp = actor->getComponent<MaterialComponent>();
                        if (matComp) col = matComp->getMaterial().baseColor;
                    }
                    // Encode as sRGB: texture format is R8G8B8A8_SRGB, so Vulkan
                    // converts sRGB→linear on sample. We encode linear→sRGB here so
                    // the sampled result matches the original linear color.
                    auto linearToSRGB = [](float v) -> uint8_t {
                        float s = (v <= 0.0031308f) ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                        return static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
                    };
                    solidColorPixels.push_back({
                        linearToSRGB(col.r), linearToSRGB(col.g), linearToSRGB(col.b), 255
                    });
                    texLayer = static_cast<int>(allTextures.size());
                    CollectedTexture ct;
                    ct.pixels = solidColorPixels.back().data();
                    ct.width = 1;
                    ct.height = 1;
                    allTextures.push_back(ct);
                }

                globalMatTexLayer.push_back(texLayer);

                // Collect normal texture (goes into same allTextures array)
                int normalTexLayer = -1;
                if (matIdx < model->materialNormalTexIndex.size()) {
                    int nTexIdx = model->materialNormalTexIndex[matIdx];
                    if (nTexIdx >= 0 && nTexIdx < static_cast<int>(model->normalTextures.size())) {
                        const auto& ntd = model->normalTextures[nTexIdx];
                        if (!ntd.pixels.empty() && ntd.width > 0 && ntd.height > 0) {
                            normalTexLayer = static_cast<int>(allTextures.size());
                            CollectedTexture ct;
                            ct.pixels = ntd.pixels.data();
                            ct.width = ntd.width;
                            ct.height = ntd.height;
                            allTextures.push_back(ct);
                        }
                    }
                }
                globalNormalTexLayer.push_back(normalTexLayer);

                // Collect roughness+metallic texture
                int rmTexLayer = -1;
                if (matIdx < model->materialRoughMetalTexIndex.size()) {
                    int rmIdx = model->materialRoughMetalTexIndex[matIdx];
                    if (rmIdx >= 0 && rmIdx < static_cast<int>(model->roughMetalTextures.size())) {
                        const auto& rmtd = model->roughMetalTextures[rmIdx];
                        if (!rmtd.pixels.empty() && rmtd.width > 0 && rmtd.height > 0) {
                            rmTexLayer = static_cast<int>(allTextures.size());
                            CollectedTexture ct;
                            ct.pixels = rmtd.pixels.data();
                            ct.width = rmtd.width;
                            ct.height = rmtd.height;
                            allTextures.push_back(ct);
                        }
                    }
                }
                globalRoughMetalTexLayer.push_back(rmTexLayer);

                // Collect emissive texture
                int emTexLayer = -1;
                if (matIdx < model->materialEmissiveTexIndex.size()) {
                    int emIdx = model->materialEmissiveTexIndex[matIdx];
                    if (emIdx >= 0 && emIdx < static_cast<int>(model->emissiveTextures.size())) {
                        const auto& etd = model->emissiveTextures[emIdx];
                        if (!etd.pixels.empty() && etd.width > 0 && etd.height > 0) {
                            emTexLayer = static_cast<int>(allTextures.size());
                            CollectedTexture ct;
                            ct.pixels = etd.pixels.data();
                            ct.width = etd.width;
                            ct.height = etd.height;
                            allTextures.push_back(ct);
                        }
                    }
                }
                globalEmissiveTexLayer.push_back(emTexLayer);
            }
        }

        if (!allTextures.empty()) {
            const uint32_t targetW = 1024;
            const uint32_t targetH = 1024;
            uint32_t numTextures = static_cast<uint32_t>(allTextures.size());
            m_rtTextureCount = numTextures;

            // Create VkImage (2D array)
            VkImageCreateInfo imgInfo{};
            imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
            imgInfo.extent = {targetW, targetH, 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = numTextures;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            if (vkCreateImage(m_device, &imgInfo, nullptr, &m_rtTextureArray) == VK_SUCCESS) {
                VkMemoryRequirements memReqs;
                vkGetImageMemoryRequirements(m_device, m_rtTextureArray, &memReqs);
                VkMemoryAllocateInfo allocInfo{};
                allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocInfo.allocationSize = memReqs.size;
                allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                vkAllocateMemory(m_device, &allocInfo, nullptr, &m_rtTextureArrayMemory);
                vkBindImageMemory(m_device, m_rtTextureArray, m_rtTextureArrayMemory, 0);

                // Create image view (2D array)
                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = m_rtTextureArray;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = numTextures;
                vkCreateImageView(m_device, &viewInfo, nullptr, &m_rtTextureArrayView);

                // Create sampler
                VkSamplerCreateInfo samplerInfo{};
                samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                samplerInfo.magFilter = VK_FILTER_LINEAR;
                samplerInfo.minFilter = VK_FILTER_LINEAR;
                samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                samplerInfo.anisotropyEnable = VK_FALSE;
                samplerInfo.maxAnisotropy = 1.0f;
                samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
                samplerInfo.unnormalizedCoordinates = VK_FALSE;
                samplerInfo.compareEnable = VK_FALSE;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                vkCreateSampler(m_device, &samplerInfo, nullptr, &m_rtTextureSampler);

                // Upload each texture layer via staging buffer + command buffer
                VkDeviceSize layerSize = targetW * targetH * 4;

                // Allocate a one-time command buffer for all texture uploads
                VkCommandBuffer uploadCmd;
                VkCommandBufferAllocateInfo cmdAllocInfo{};
                cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cmdAllocInfo.commandPool = m_commandPool;
                cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cmdAllocInfo.commandBufferCount = 1;
                vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &uploadCmd);

                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(uploadCmd, &beginInfo);

                // Transition entire array to TRANSFER_DST_OPTIMAL
                VkImageMemoryBarrier toDst{};
                toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toDst.srcAccessMask = 0;
                toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.image = m_rtTextureArray;
                toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, numTextures};
                vkCmdPipelineBarrier(uploadCmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &toDst);

                // Create one staging buffer large enough for one layer
                VkBuffer stagingBuf;
                VkDeviceMemory stagingMem;
                VkBufferCreateInfo stagingBufInfo{};
                stagingBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                stagingBufInfo.size = layerSize;
                stagingBufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                vkCreateBuffer(m_device, &stagingBufInfo, nullptr, &stagingBuf);

                VkMemoryRequirements stagingReqs;
                vkGetBufferMemoryRequirements(m_device, stagingBuf, &stagingReqs);
                VkMemoryAllocateInfo stagingAlloc{};
                stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                stagingAlloc.allocationSize = stagingReqs.size;
                stagingAlloc.memoryTypeIndex = findMemoryType(m_physicalDevice, stagingReqs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(m_device, &stagingAlloc, nullptr, &stagingMem);
                vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

                // Upload each texture layer one at a time (reusing staging buffer)
                for (uint32_t layer = 0; layer < numTextures; layer++) {
                    const auto& tex = allTextures[layer];
                    int srcW = tex.width;
                    int srcH = tex.height;

                    // Resize to targetW x targetH if needed (nearest-neighbor)
                    std::vector<uint8_t> resized;
                    const uint8_t* srcPixels = tex.pixels;
                    if (srcW != static_cast<int>(targetW) || srcH != static_cast<int>(targetH)) {
                        resized.resize(targetW * targetH * 4);
                        for (uint32_t y = 0; y < targetH; y++) {
                            for (uint32_t x = 0; x < targetW; x++) {
                                int sx = x * srcW / targetW;
                                int sy = y * srcH / targetH;
                                int srcIdx = (sy * srcW + sx) * 4;
                                int dstIdx = (y * targetW + x) * 4;
                                memcpy(&resized[dstIdx], &tex.pixels[srcIdx], 4);
                            }
                        }
                        srcPixels = resized.data();
                    }

                    // We need to submit each layer separately because we reuse the staging buffer
                    // First, end current command buffer, submit, wait, then start new one
                    if (layer > 0) {
                        vkEndCommandBuffer(uploadCmd);
                        VkSubmitInfo submitInfo{};
                        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                        submitInfo.commandBufferCount = 1;
                        submitInfo.pCommandBuffers = &uploadCmd;
                        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                        vkQueueWaitIdle(m_graphicsQueue);

                        vkResetCommandBuffer(uploadCmd, 0);
                        vkBeginCommandBuffer(uploadCmd, &beginInfo);
                    }

                    // Copy pixels to staging buffer
                    void* mapped;
                    vkMapMemory(m_device, stagingMem, 0, layerSize, 0, &mapped);
                    memcpy(mapped, srcPixels, layerSize);
                    vkUnmapMemory(m_device, stagingMem);

                    // Copy staging buffer to image array layer
                    VkBufferImageCopy region{};
                    region.bufferOffset = 0;
                    region.bufferRowLength = 0;
                    region.bufferImageHeight = 0;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = 0;
                    region.imageSubresource.baseArrayLayer = layer;
                    region.imageSubresource.layerCount = 1;
                    region.imageOffset = {0, 0, 0};
                    region.imageExtent = {targetW, targetH, 1};

                    vkCmdCopyBufferToImage(uploadCmd, stagingBuf, m_rtTextureArray,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                }

                // Transition entire array to SHADER_READ_ONLY_OPTIMAL
                VkImageMemoryBarrier toShader{};
                toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.image = m_rtTextureArray;
                toShader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, numTextures};
                vkCmdPipelineBarrier(uploadCmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0, 0, nullptr, 0, nullptr, 1, &toShader);

                // Submit final batch
                vkEndCommandBuffer(uploadCmd);
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &uploadCmd;
                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
                vkQueueWaitIdle(m_graphicsQueue);

                // Cleanup staging buffer
                vkDestroyBuffer(m_device, stagingBuf, nullptr);
                vkFreeMemory(m_device, stagingMem, nullptr);
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);

                // Create per-layer VkImageViews for bindless access
                std::vector<VkImageView> bindlessViews(numTextures);
                std::vector<VkSampler> bindlessSamplers(numTextures);
                for (uint32_t layer = 0; layer < numTextures; layer++) {
                    VkImageViewCreateInfo viewInfo{};
                    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    viewInfo.image = m_rtTextureArray;
                    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;  // single layer as 2D
                    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
                    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    viewInfo.subresourceRange.baseMipLevel = 0;
                    viewInfo.subresourceRange.levelCount = 1;
                    viewInfo.subresourceRange.baseArrayLayer = layer;
                    viewInfo.subresourceRange.layerCount = 1;
                    vkCreateImageView(m_device, &viewInfo, nullptr, &bindlessViews[layer]);
                    bindlessSamplers[layer] = m_rtTextureSampler;
                }
                m_pathTracer->setBindlessTextures(bindlessViews, bindlessSamplers);

                // Update material buffer: encode texture indices as uint bits
                // Shader uses floatBitsToUint() to decode, 0xFFFFFFFF = no texture
                // Encode texture indices as uint bits in material buffer (3 vec4s per material)
                if (m_rtMatColorBuffer && m_rtMatColorMemory && !globalMatTexLayer.empty()) {
                    size_t matCount = globalMatTexLayer.size();
                    VkDeviceSize matColorSize = matCount * 3 * sizeof(glm::vec4);
                    void* matMapped;
                    vkMapMemory(m_device, m_rtMatColorMemory, 0, matColorSize, 0, &matMapped);
                    glm::vec4* matColors = static_cast<glm::vec4*>(matMapped);

                    auto packIdx = [](int idx) -> float {
                        uint32_t u = (idx >= 0) ? static_cast<uint32_t>(idx) : 0xFFFFFFFFu;
                        float f; memcpy(&f, &u, sizeof(float)); return f;
                    };

                    for (size_t i = 0; i < matCount; i++) {
                        matColors[i * 3 + 0].a = packIdx(globalMatTexLayer[i]);           // diffuseTexIdx
                        matColors[i * 3 + 1].z = packIdx(i < globalNormalTexLayer.size() ? globalNormalTexLayer[i] : -1);     // normalTexIdx
                        matColors[i * 3 + 1].w = packIdx(i < globalEmissiveTexLayer.size() ? globalEmissiveTexLayer[i] : -1);
                        matColors[i * 3 + 2].x = packIdx(i < globalRoughMetalTexLayer.size() ? globalRoughMetalTexLayer[i] : -1); // roughMetalTexIdx
                    }
                    vkUnmapMemory(m_device, m_rtMatColorMemory);
                }

                std::cout << "[RT] Bindless textures: " << numTextures
                          << " at " << targetW << "x" << targetH << std::endl;
            }
        } else {
            std::cout << "[RT] No textures found in scene models" << std::endl;
        }
    }

}

void OffscreenRenderer::buildBLASTLAS() {
    // Rebuild acceleration structures using the RT buffers
    m_rtAccel->destroy();
    m_rtAccel->init(m_device, m_physicalDevice, m_graphicsQueue, m_graphicsQueueFamily, m_commandPool);

    std::unordered_map<uint64_t, BlasHandle> actorBlas;
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto it = m_meshBufferMap.find(actorId);
        if (it == m_meshBufferMap.end()) continue;
        const MeshBufferInfo& meshInfo = it->second;
        if (meshInfo.indexCount == 0) continue;

        VkDeviceSize vertexByteOffset = meshInfo.vertexOffset * sizeof(Vertex);
        VkDeviceSize indexByteOffset = meshInfo.indexOffset * sizeof(uint32_t);

        std::cout << "[RT] BLAS for " << actor->getName()
                  << ": vOff=" << meshInfo.vertexOffset << " (" << vertexByteOffset << " bytes)"
                  << " iOff=" << meshInfo.indexOffset << " (" << indexByteOffset << " bytes)"
                  << " vCount=" << meshInfo.vertexCount << " iCount=" << meshInfo.indexCount << std::endl;

        BlasHandle blas = m_rtAccel->createBLAS(
            m_rtVertexBuffer, meshInfo.vertexCount, sizeof(Vertex),
            m_rtIndexBuffer, meshInfo.indexCount,
            vertexByteOffset, indexByteOffset, VK_NULL_HANDLE);

        if (blas != INVALID_BLAS) actorBlas[actorId] = blas;
    }

    // Build TLAS instances + collect materials in the SAME order
    m_rtAccel->clearInstances();
    std::vector<glm::vec3> materialAlbedos;
    std::vector<glm::vec4> materialFullData;
    uint32_t instanceIdx = 0;
    uint32_t globalTriOffset = 0;
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto blasIt = actorBlas.find(actorId);
        if (blasIt == actorBlas.end()) continue;
        // customIndex = global triangle offset for material ID lookup
        m_rtAccel->addInstance(blasIt->second, actor->getTransform()->getWorldMatrix(), globalTriOffset);

        // Collect albedo in same order
        auto matComp = actor->getComponent<MaterialComponent>();
        glm::vec3 albedo(0.73f);
        float roughness = 0.95f;
        float metallic = 0.0f;
        if (matComp) {
            albedo = matComp->getMaterial().baseColor;
            roughness = matComp->getMaterial().roughness;
            metallic = matComp->getMaterial().metallic;
        }
        // Detect sphere vs cube from mesh vertex count
        // Sphere meshes have many more vertices than 24 (cube = 24)
        bool isSphereShape = false;
        auto meshComp2 = actor->getComponent<MeshComponent>();
        if (meshComp2 && meshComp2->getModel()) {
            isSphereShape = meshComp2->getModel()->vertices.size() > 100;
        }
        // Pack into vec4: (r, g, b, packed)
        // packed encoding: sign = metallic, magnitude = roughness
        // Add 10.0 if sphere shape (roughness is always < 1, so 10+ means sphere)
        float packed = metallic > 0.5f ? -(roughness + 0.001f) : roughness;
        if (isSphereShape) packed += (packed >= 0 ? 10.0f : -10.0f);
        materialAlbedos.push_back(glm::vec3(albedo.r, albedo.g, albedo.b));
        // Store full material data for path tracer
        materialFullData.push_back(glm::vec4(albedo, packed));
        std::cout << "[RT] Instance " << instanceIdx << " (" << actor->getName()
                  << "): albedo=(" << albedo.r << "," << albedo.g << "," << albedo.b
                  << ") rough=" << roughness << " metal=" << metallic << std::endl;
        // Advance triangle offset for next instance
        auto it2 = m_meshBufferMap.find(actorId);
        if (it2 != m_meshBufferMap.end()) {
            globalTriOffset += it2->second.indexCount / 3;
        }
        instanceIdx++;
    }

    if (m_rtAccel->getInstanceCount() > 0) {
        m_rtAccel->buildTLAS(VK_NULL_HANDLE);
        std::cout << "[RT] Acceleration structures built: "
                  << m_rtAccel->getBlasCount() << " BLAS, "
                  << m_rtAccel->getInstanceCount() << " instances" << std::endl;

        // Pass material albedos to RT techniques
        if (m_deferredRenderer) {
            auto* gi = m_deferredRenderer->getRT_GI();
            if (gi) gi->setMaterialAlbedos(materialAlbedos);
        }
        if (m_pathTracer) {
            m_pathTracer->setMaterialData(materialFullData);
            if (m_rtNormalBuffer != VK_NULL_HANDLE && m_rtIndexBuffer != VK_NULL_HANDLE) {
                m_pathTracer->setNormalBuffer(m_rtNormalBuffer, m_rtIndexBuffer, m_vertexCount);
            }
            if (m_rtUVBuffer) m_pathTracer->setUVBuffer(m_rtUVBuffer);
            if (m_rtMatIDBuffer && m_rtMatColorBuffer) {
                m_pathTracer->setMaterialBuffers(m_rtMatIDBuffer, m_rtMatColorBuffer);
            }
            if (m_rtTextureArrayView && m_rtTextureSampler && m_rtTextureCount > 0) {
                m_pathTracer->setTextureArray(m_rtTextureArrayView, m_rtTextureSampler, m_rtTextureCount);
            }
        }
    }
    }

} // namespace ohao
