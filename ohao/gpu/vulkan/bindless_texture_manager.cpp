#include "bindless_texture_manager.hpp"
#include "gpu/vulkan/gpu_fault.hpp"
#include "../../gpu/vulkan/gpu_allocator.hpp"
#include <algorithm>
#include <cmath>
#include "core/log.hpp"
#include <cstring>
#include <string_view>
#include <span>

#include <stb_image.h>   // implemented once, external/stb_image.cpp

namespace ohao {

namespace {
// The bytes of a texture's pixels over its mips (every texture is 4 bytes a pixel).
bool blockFormat(VkFormat f) {
    return f == VK_FORMAT_BC7_SRGB_BLOCK || f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC5_UNORM_BLOCK;
}
// One level's bytes: 4 a pixel, or 16 a 4x4 block (the three block formats the cooker writes).
size_t levelBytes(VkFormat format, uint32_t w, uint32_t h) {
    return blockFormat(format) ? size_t((w + 3) / 4) * ((h + 3) / 4) * 16 : size_t(w) * h * 4;
}
size_t pixelBytes(const BindlessTextureInfo& t) {
    size_t bytes = 0;
    for (uint32_t m = 0; m < t.mipLevels; ++m)
        bytes += levelBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
    return bytes;
}
} // namespace

BindlessTextureManager::~BindlessTextureManager() {
    cleanup();
}

bool BindlessTextureManager::initialize(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
                                          uint32_t maxTextures, uint32_t graphicsQueueFamily, VkQueue graphicsQueue) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_maxTextures = maxTextures;
    if (!m_allocator.initialize(instance, physicalDevice, device)) {
        OHAO_LOG(Error, gpu, "BindlessTextureManager: the allocator could not be made");
        return false;
    }

    // Reserve texture slots
    m_textures.resize(maxTextures);

    // Initialize free slots (all slots are free initially)
    m_freeSlots.reserve(maxTextures);
    for (uint32_t i = 0; i < maxTextures; ++i) {
        m_freeSlots.push_back(maxTextures - 1 - i);  // Push in reverse for LIFO
    }

    // Create command pool for texture uploads
    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = graphicsQueueFamily,
    };

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create bindless texture command pool");
        return false;
    }

    // Get graphics queue if not provided
    if (graphicsQueue != VK_NULL_HANDLE) {
        m_graphicsQueue = graphicsQueue;
    } else {
        vkGetDeviceQueue(m_device, graphicsQueueFamily, 0, &m_graphicsQueue);
    }

    if (!createDescriptorResources()) {
        return false;
    }

    if (!createDefaultTextures()) {
        return false;
    }

    OHAO_LOG(Info, gpu, "BindlessTextureManager initialized with capacity: {}", maxTextures);
    return true;
}

void BindlessTextureManager::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Destroy all loaded textures
    for (auto& tex : m_textures) {
        if (tex.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, tex.view, nullptr);
        }
        if (tex.image != VK_NULL_HANDLE) {
            GpuImage image{tex.image, tex.allocation};
            m_allocator.destroyImage(image);
        }
    }
    m_textures.clear();
    m_pending.clear();
    m_allocator.shutdown();

    if (m_defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_defaultSampler, nullptr);
        m_defaultSampler = VK_NULL_HANDLE;
    }

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    m_pathToHandle.clear();
    m_nameToHandle.clear();
    m_byContent.clear();
    m_contentOfSlot.clear();
    m_retired.clear();
    m_freeSlots.clear();
    m_loadedCount = 0;
    m_totalMemoryUsage = 0;
    m_device = VK_NULL_HANDLE;
}

bool BindlessTextureManager::createDescriptorResources() {
    // Create sampler
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 16.0f,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_defaultSampler) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create bindless texture sampler");
        return false;
    }

    // Create descriptor set layout for bindless textures
    // Uses VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT and UPDATE_AFTER_BIND
    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

    // pNext feature/flags chain: keep explicit assignment for readability
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = kBinding;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = m_maxTextures;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create bindless descriptor set layout");
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = m_maxTextures,
    };

    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to create bindless descriptor pool");
        return false;
    }

    // Allocate descriptor set with variable count
    uint32_t variableCount = m_maxTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &variableCount;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = &variableInfo;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        OHAO_LOG(Error, gpu, "Failed to allocate bindless descriptor set");
        return false;
    }

    return true;
}

bool BindlessTextureManager::createDefaultTextures() {
    // Default white texture (1x1 white pixel)
    if (!createSolidColorTexture(0xFFFFFFFF, m_defaultWhite, "default_white")) {
        return false;
    }
    m_textures[m_defaultWhite.index].type = BindlessTextureType::Albedo;
    m_textures[m_defaultWhite.index].persistent = true;

    // Default black texture (1x1 black pixel)
    if (!createSolidColorTexture(0xFF000000, m_defaultBlack, "default_black")) {
        return false;
    }
    m_textures[m_defaultBlack.index].type = BindlessTextureType::AO;
    m_textures[m_defaultBlack.index].persistent = true;

    // Default normal texture (flat normal)
    if (!createDefaultNormalTexture()) {
        return false;
    }

    updateDescriptorSet();

    OHAO_LOG(Debug, gpu, "Default bindless textures created");
    return true;
}

bool BindlessTextureManager::createSolidColorTexture(uint32_t color, BindlessTextureHandle& outHandle,
                                                       std::string_view name) {
    const auto pixels = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&color), sizeof(color));
    const uint32_t slot = addTexture({pixels, 1, 1, nullptr}, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Custom,
                                     false, std::string(name));
    if (slot == UINT32_MAX) return false;
    flushUploads();
    outHandle.index = slot;
    m_nameToHandle[std::string(name)] = outHandle;
    return true;
}

bool BindlessTextureManager::createDefaultNormalTexture() {
    // Default flat normal (pointing up in tangent space): (0.5, 0.5, 1.0) = 0xFFFF8080
    const uint32_t normalColor = 0xFFFF8080;
    const auto pixels = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&normalColor), sizeof(normalColor));
    const uint32_t slot = addTexture({pixels, 1, 1, nullptr}, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Normal,
                                     false, "default_normal");
    if (slot == UINT32_MAX) return false;
    flushUploads();
    m_textures[slot].persistent = true;
    m_defaultNormal.index = slot;
    m_nameToHandle["default_normal"] = m_defaultNormal;
    return true;
}

uint32_t BindlessTextureManager::allocateSlot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_freeSlots.empty()) {
        OHAO_LOG(Error, gpu, "No free texture slots available");
        return UINT32_MAX;
    }

    uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    return slot;
}

void BindlessTextureManager::freeSlot(uint32_t slot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_freeSlots.push_back(slot);
}

BindlessTextureHandle BindlessTextureManager::loadTexture(std::string_view path,
                                                            BindlessTextureType type,
                                                            bool generateMips) {
    // Check if already loaded
    auto it = m_pathToHandle.find(std::string(path));
    if (it != m_pathToHandle.end()) {
        return it->second;
    }

    std::vector<uint8_t> data;
    uint32_t width, height;
    VkFormat format;
    if (!loadTextureData(path, type, data, width, height, format)) {
        OHAO_LOG(Error, gpu, "Failed to load texture: {}", path);
        return getDefaultTexture(type);
    }
    const uint32_t slot = addTexture({data, width, height, nullptr}, format, type, generateMips, std::string(path));
    if (slot == UINT32_MAX) return getDefaultTexture(type);
    flushUploads();
    const BindlessTextureHandle handle{slot};
    m_pathToHandle[std::string(path)] = handle;
    return handle;
}

BindlessTextureHandle BindlessTextureManager::loadTextureFromMemory(std::span<const uint8_t> data,
                                                                      uint32_t width, uint32_t height,
                                                                      VkFormat format,
                                                                      BindlessTextureType type,
                                                                      bool generateMips) {
    const uint32_t slot = addTexture({data, width, height, nullptr}, format, type, generateMips, {});
    if (slot == UINT32_MAX) return getDefaultTexture(type);
    flushUploads();
    return BindlessTextureHandle{slot};
}

ContentTexture BindlessTextureManager::contentTexture(const TextureContentKey& key,
                                                      const std::function<TexturePixels()>& open) {
    if (const auto it = m_byContent.find(key); it != m_byContent.end()) {
        ++it->second.holds;
        return it->second.texture;
    }
    TexturePixels px = open();
    if (px.levels.empty()) return {getDefaultTexture(key.type), {}};
    ContentTexture made;
    made.channelMin = px.channelMin;
    const uint32_t slot = addTexture(std::move(px), key.format, key.type, true, {});
    if (slot == UINT32_MAX) return {getDefaultTexture(key.type), made.channelMin};   // not remembered: asked again
    made.handle = BindlessTextureHandle{slot};
    m_byContent.emplace(key, ContentEntry{made, 1});
    m_contentOfSlot.emplace(slot, key);
    return made;
}

void BindlessTextureManager::releaseContent(BindlessTextureHandle handle) {
    const auto slot = m_contentOfSlot.find(handle.index);
    if (slot == m_contentOfSlot.end()) return;
    const auto it = m_byContent.find(slot->second);
    if (it == m_byContent.end() || --it->second.holds > 0) return;
    m_byContent.erase(it);
    m_contentOfSlot.erase(slot);
    std::erase_if(m_pending, [&](const PendingUpload& u) {
        if (u.slot != handle.index) return false;
        m_pendingBytes -= u.bytes;
        return true;
    });
    m_retired.push_back({m_frame + kRetireFrames, handle.index});
}

void BindlessTextureManager::onFrameRetired() {
    ++m_frame;
    std::erase_if(m_retired, [&](const RetiredTexture& r) {
        if (r.at > m_frame) return false;
        destroyTexture(r.slot);
        return true;
    });
}

void BindlessTextureManager::destroyTexture(uint32_t slot) {
    auto& tex = m_textures[slot];
    if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(m_device, tex.view, nullptr);
    if (tex.image != VK_NULL_HANDLE) {
        GpuImage image{tex.image, tex.allocation};
        m_allocator.destroyImage(image);
        m_totalMemoryUsage -= pixelBytes(tex);
    }
    tex = BindlessTextureInfo{};
    freeSlot(slot);
    m_loadedCount--;
    // The table's entry must not name a destroyed view: it shows the default until the slot is used again.
    const VkDescriptorImageInfo white{m_defaultSampler, m_textures[m_defaultWhite.index].view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = kBinding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &white;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

uint32_t BindlessTextureManager::addTexture(TexturePixels px, VkFormat format, BindlessTextureType type, bool mips,
                                            std::string name) {
    // Every level given (a cooked texture's), or level 0 as RGBA8 and the rest made by blits.
    uint32_t levels = 1;
    std::size_t bytes = 0;
    if (!px.levels.empty()) {
        levels = static_cast<uint32_t>(px.levels.size());
        for (uint32_t m = 0; m < levels; ++m) {
            if (px.levels[m].size() != levelBytes(format, std::max(1u, px.width >> m), std::max(1u, px.height >> m))) {
                OHAO_LOG(Error, gpu, "a texture's level {} is not the size of its format", m);
                return UINT32_MAX;
            }
            bytes += px.levels[m].size();
        }
    } else {
        bytes = static_cast<std::size_t>(px.width) * px.height * 4;
        if (bytes == 0 || px.rgba8.size() < bytes || blockFormat(format)) {
            OHAO_LOG(Error, gpu, "a texture's pixels are fewer than width x height x 4");
            return UINT32_MAX;
        }
        if (mips) levels = static_cast<uint32_t>(std::floor(std::log2(std::max(px.width, px.height)))) + 1;
    }
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {px.width, px.height, 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    GpuImage image = m_allocator.createImage(imageInfo, AllocationUsage::GpuOnly);
    if (!image.isValid()) return UINT32_MAX;
    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1},
    };
    VkImageView view = VK_NULL_HANDLE;
    const uint32_t slot = vkCreateImageView(m_device, &viewInfo, nullptr, &view) == VK_SUCCESS ? allocateSlot() : UINT32_MAX;
    if (slot == UINT32_MAX) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(m_device, view, nullptr);
        m_allocator.destroyImage(image);
        return UINT32_MAX;
    }
    auto& tex = m_textures[slot];
    tex.image = image.image;
    tex.allocation = image.allocation;
    tex.view = view;
    tex.width = px.width;
    tex.height = px.height;
    tex.mipLevels = levels;
    tex.format = format;
    tex.type = type;
    tex.name = name.empty() ? "memory_" + std::to_string(slot) : std::move(name);
    m_loadedCount++;
    m_totalMemoryUsage += pixelBytes(tex);
    m_pending.push_back({slot, std::move(px), bytes});
    // A long load uploads as it goes: what waits is at most one staging buffer of mapped pixels.
    m_pendingBytes += bytes;
    if (m_pendingBytes >= kStaging) uploadPending();
    return slot;
}

void BindlessTextureManager::flushUploads() {
    uploadPending();
    if (m_tableStale) updateDescriptorSet();
    m_tableStale = false;
}

void BindlessTextureManager::uploadPending() {
    if (m_pending.empty()) return;
    // One staging buffer and one submit for as many textures as fit kStaging (a larger one goes alone): a load's
    // textures are a few submits, each bounded.
    const auto bytesOf = [](const PendingUpload& u) { return static_cast<VkDeviceSize>((u.bytes + 15) & ~std::size_t(15)); };
    for (std::size_t first = 0; first < m_pending.size();) {
        VkDeviceSize size = bytesOf(m_pending[first]);
        std::size_t last = first + 1;
        for (; last < m_pending.size() && size + bytesOf(m_pending[last]) <= kStaging; ++last) size += bytesOf(m_pending[last]);

        GpuBuffer staging = m_allocator.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, AllocationUsage::CpuToGpu, true);
        if (!staging.isValid() || !staging.getMappedData()) {
            OHAO_LOG(Error, gpu, "texture upload: no staging buffer of {} bytes; {} textures are left without pixels", size,
                     m_pending.size() - first);
            if (staging.isValid()) m_allocator.destroyBuffer(staging);
            break;
        }
        const VkCommandBufferAllocateInfo cmdInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd);
        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &beginInfo);

        auto* mapped = static_cast<uint8_t*>(staging.getMappedData());
        VkDeviceSize offset = 0;
        for (std::size_t i = first; i < last; ++i) {
            const PendingUpload& u = m_pending[i];
            if (u.pixels.levels.empty()) {
                std::memcpy(mapped + offset, u.pixels.rgba8.data(), u.bytes);
            } else {
                VkDeviceSize at = offset;
                for (const auto level : u.pixels.levels) {
                    std::memcpy(mapped + at, level.data(), level.size());
                    at += level.size();
                }
            }
            recordUpload(cmd, staging.buffer, offset, m_textures[u.slot], !u.pixels.levels.empty());
            offset += bytesOf(u);
        }
        m_allocator.flushBuffer(staging);
        vkEndCommandBuffer(cmd);
        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        gpu::submitAndWait(m_device, m_graphicsQueue, submitInfo, "texture upload");
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        m_allocator.destroyBuffer(staging);
        first = last;
    }
    m_pending.clear();
    m_pendingBytes = 0;
    m_tableStale = true;
}

bool BindlessTextureManager::updateTextureFromMemory(BindlessTextureHandle handle,
                                                     std::span<const uint8_t> data,
                                                     uint32_t width, uint32_t height) {
    if (!handle.valid() || handle.index >= m_textures.size()) return false;
    auto& tex = m_textures[handle.index];
    if (tex.image == VK_NULL_HANDLE || tex.width != width || tex.height != height) return false;

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4u;
    if (data.size_bytes() < static_cast<size_t>(imageSize)) return false;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReqs);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memTypeIndex = i;
            break;
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        return false;
    }
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex,
    };
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        return false;
    }
    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, data.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);

    VkCommandBufferAllocateInfo cmdAllocInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmd);
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    gpu::submitAndWait(m_device, m_graphicsQueue, submitInfo, "texture upload");
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);
    return true;
}

BindlessTextureHandle BindlessTextureManager::registerExternalTexture(VkImageView view,
                                                                        std::string_view name,
                                                                        BindlessTextureType type) {
    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        return BindlessTextureHandle{UINT32_MAX};
    }

    auto& tex = m_textures[slot];
    tex.view = view;
    tex.image = VK_NULL_HANDLE;  // External, don't destroy
    tex.type = type;
    tex.name = std::string(name);

    BindlessTextureHandle handle{slot};
    m_nameToHandle[std::string(name)] = handle;
    m_loadedCount++;

    updateDescriptorSet();

    return handle;
}

void BindlessTextureManager::unloadTexture(BindlessTextureHandle handle) {
    if (!handle.valid() || handle.index >= m_textures.size()) return;

    auto& tex = m_textures[handle.index];
    if (tex.persistent) return;  // Don't unload persistent textures

    // Ensure GPU is done sampling this image before destroy (bindless UPDATE_AFTER_BIND).
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    // Remove from maps (logical name may differ from tex.name for registerName aliases —
    // erase both memory_* and any key currently pointing at this handle).
    if (!tex.name.empty()) {
        m_nameToHandle.erase(tex.name);
        m_pathToHandle.erase(tex.name);
    }
    for (auto it = m_nameToHandle.begin(); it != m_nameToHandle.end();) {
        if (it->second.index == handle.index) it = m_nameToHandle.erase(it);
        else ++it;
    }
    for (auto it = m_pathToHandle.begin(); it != m_pathToHandle.end();) {
        if (it->second.index == handle.index) it = m_pathToHandle.erase(it);
        else ++it;
    }

    std::erase_if(m_byContent, [&](const auto& entry) { return entry.second.texture.handle == handle; });
    m_contentOfSlot.erase(handle.index);
    std::erase_if(m_retired, [&](const RetiredTexture& r) { return r.slot == handle.index; });
    std::erase_if(m_pending, [&](const PendingUpload& u) { return u.slot == handle.index; });

    // Destroy resources
    if (tex.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, tex.view, nullptr);
    }
    if (tex.image != VK_NULL_HANDLE) {
        GpuImage image{tex.image, tex.allocation};
        m_allocator.destroyImage(image);
        m_totalMemoryUsage -= pixelBytes(tex);
    }

    tex = BindlessTextureInfo{};  // Reset

    freeSlot(handle.index);
    m_loadedCount--;
}

const BindlessTextureInfo* BindlessTextureManager::getTextureInfo(BindlessTextureHandle handle) const {
    if (!handle.valid() || handle.index >= m_textures.size()) return nullptr;
    return &m_textures[handle.index];
}

BindlessTextureHandle BindlessTextureManager::findTexture(std::string_view key) const {
    if (key.empty()) return BindlessTextureHandle{UINT32_MAX};

    const std::string keyStr(key);

    // Check name map first (registerName)
    auto it = m_nameToHandle.find(keyStr);
    if (it != m_nameToHandle.end()) return it->second;

    // Check path map (used by normal loading)
    it = m_pathToHandle.find(keyStr);
    if (it != m_pathToHandle.end()) return it->second;

    return BindlessTextureHandle{UINT32_MAX};
}

BindlessTextureHandle BindlessTextureManager::getTextureByName(std::string_view name) const {
    auto it = m_nameToHandle.find(std::string(name));
    return it != m_nameToHandle.end() ? it->second : BindlessTextureHandle{UINT32_MAX};
}

BindlessTextureHandle BindlessTextureManager::getTextureByPath(std::string_view path) const {
    auto it = m_pathToHandle.find(std::string(path));
    return it != m_pathToHandle.end() ? it->second : BindlessTextureHandle{UINT32_MAX};
}

void BindlessTextureManager::setTexturePersistent(BindlessTextureHandle handle, bool persistent) {
    if (handle.valid() && handle.index < m_textures.size()) {
        m_textures[handle.index].persistent = persistent;
    }
}

BindlessTextureHandle BindlessTextureManager::getDefaultTexture(BindlessTextureType type) const {
    switch (type) {
        case BindlessTextureType::Normal:
            return m_defaultNormal;
        case BindlessTextureType::AO:
        case BindlessTextureType::Metallic:
        case BindlessTextureType::Roughness:
            return m_defaultBlack;
        default:
            return m_defaultWhite;
    }
}

void BindlessTextureManager::updateDescriptorSet() {
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSet> writes;
    imageInfos.reserve(m_textures.size());
    writes.reserve(m_textures.size());

    for (uint32_t i = 0; i < m_textures.size(); ++i) {
        if (m_textures[i].view == VK_NULL_HANDLE) continue;

        imageInfos.push_back(VkDescriptorImageInfo{
            m_defaultSampler,
            m_textures[i].view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        });

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSet;
        write.dstBinding = kBinding;
        write.dstArrayElement = i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfos.back();
        writes.push_back(write);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

bool BindlessTextureManager::loadTextureData(std::string_view path, BindlessTextureType type, std::vector<uint8_t>& outData,
                                               uint32_t& width, uint32_t& height, VkFormat& format) {
    int w, h, channels;
    const std::string pathStr(path);
    stbi_uc* pixels = stbi_load(pathStr.c_str(), &w, &h, &channels, STBI_rgb_alpha);

    if (!pixels) {
        return false;
    }

    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);

    // Albedo and Emissive textures are usually sRGB, while others (normal, roughness, etc.) are linear
    if (type == BindlessTextureType::Albedo || type == BindlessTextureType::Emissive) {
        format = VK_FORMAT_R8G8B8A8_SRGB;
    } else {
        format = VK_FORMAT_R8G8B8A8_UNORM;
    }

    size_t size = width * height * 4;
    outData.resize(size);
    memcpy(outData.data(), pixels, size);

    stbi_image_free(pixels);
    return true;
}

// Level 0 from `staging` at `offset`, then the mips by blits, the image left to be sampled.
void BindlessTextureManager::recordUpload(VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize offset,
                                          const BindlessTextureInfo& tex, bool everyLevel) {
    // Transition to transfer dst
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = tex.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {tex.width, tex.height, 1};

    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (everyLevel) {   // the staging buffer holds the other levels after level 0
        VkDeviceSize at = offset + levelBytes(tex.format, tex.width, tex.height);
        for (uint32_t m = 1; m < tex.mipLevels; ++m) {
            const uint32_t w = std::max(1u, tex.width >> m), h = std::max(1u, tex.height >> m);
            region.bufferOffset = at;
            region.imageSubresource.mipLevel = m;
            region.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            at += levelBytes(tex.format, w, h);
        }
    }

    // Generate mipmaps or transition to shader read
    if (tex.mipLevels > 1 && !everyLevel) {
        generateMipmaps(cmd, tex.image, tex.width, tex.height, tex.mipLevels);
    } else {
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

void BindlessTextureManager::generateMipmaps(VkCommandBuffer cmd, VkImage image,
                                               uint32_t width, uint32_t height, uint32_t mipLevels) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = static_cast<int32_t>(width);
    int32_t mipHeight = static_cast<int32_t>(height);

    for (uint32_t i = 1; i < mipLevels; i++) {
        // Transition previous mip to transfer src
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Blit from previous mip to current
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;

        int32_t nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        int32_t nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);

        // Transition previous mip to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);

        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }

    // Transition last mip to shader read
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace ohao
