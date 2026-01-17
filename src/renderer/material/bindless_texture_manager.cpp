#include "bindless_texture_manager.hpp"
#include "../memory/gpu_allocator.hpp"
#include <iostream>
#include <cstring>

// Include stb_image for texture loading
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace ohao {

BindlessTextureManager::~BindlessTextureManager() {
    cleanup();
}

bool BindlessTextureManager::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                                          GpuAllocator* allocator, uint32_t maxTextures,
                                          uint32_t graphicsQueueFamily, VkQueue graphicsQueue) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_allocator = allocator;
    m_maxTextures = maxTextures;

    // Reserve texture slots
    m_textures.resize(maxTextures);

    // Initialize free slots (all slots are free initially)
    m_freeSlots.reserve(maxTextures);
    for (uint32_t i = 0; i < maxTextures; ++i) {
        m_freeSlots.push_back(maxTextures - 1 - i);  // Push in reverse for LIFO
    }

    // Create command pool for texture uploads
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::cerr << "Failed to create bindless texture command pool" << std::endl;
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

    std::cout << "BindlessTextureManager initialized with capacity: " << maxTextures << std::endl;
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
            vkDestroyImage(m_device, tex.image, nullptr);
        }
        if (tex.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, tex.memory, nullptr);
        }
    }
    m_textures.clear();

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
    m_freeSlots.clear();
    m_loadedCount = 0;
    m_totalMemoryUsage = 0;
    m_device = VK_NULL_HANDLE;
}

bool BindlessTextureManager::createDescriptorResources() {
    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_defaultSampler) != VK_SUCCESS) {
        std::cerr << "Failed to create bindless texture sampler" << std::endl;
        return false;
    }

    // Create descriptor set layout for bindless textures
    // Uses VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT and UPDATE_AFTER_BIND
    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
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
        std::cerr << "Failed to create bindless descriptor set layout" << std::endl;
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = m_maxTextures;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::cerr << "Failed to create bindless descriptor pool" << std::endl;
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
        std::cerr << "Failed to allocate bindless descriptor set" << std::endl;
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

    std::cout << "Default bindless textures created" << std::endl;
    return true;
}

bool BindlessTextureManager::createSolidColorTexture(uint32_t color, BindlessTextureHandle& outHandle,
                                                       const std::string& name) {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t mipLevels;

    if (!createTextureImage(&color, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false,
                             image, memory, view, mipLevels)) {
        return false;
    }

    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        vkDestroyImageView(m_device, view, nullptr);
        vkDestroyImage(m_device, image, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
        return false;
    }

    auto& tex = m_textures[slot];
    tex.image = image;
    tex.view = view;
    tex.memory = memory;
    tex.width = 1;
    tex.height = 1;
    tex.mipLevels = 1;
    tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    tex.name = name;

    outHandle.index = slot;
    m_nameToHandle[name] = outHandle;
    m_loadedCount++;

    return true;
}

bool BindlessTextureManager::createDefaultNormalTexture() {
    // Default flat normal (pointing up in tangent space): (0.5, 0.5, 1.0) = 0xFFFF8080
    uint32_t normalColor = 0xFFFF8080;

    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t mipLevels;

    if (!createTextureImage(&normalColor, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false,
                             image, memory, view, mipLevels)) {
        return false;
    }

    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        vkDestroyImageView(m_device, view, nullptr);
        vkDestroyImage(m_device, image, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
        return false;
    }

    auto& tex = m_textures[slot];
    tex.image = image;
    tex.view = view;
    tex.memory = memory;
    tex.width = 1;
    tex.height = 1;
    tex.mipLevels = 1;
    tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    tex.type = BindlessTextureType::Normal;
    tex.name = "default_normal";
    tex.persistent = true;

    m_defaultNormal.index = slot;
    m_nameToHandle["default_normal"] = m_defaultNormal;
    m_loadedCount++;

    return true;
}

uint32_t BindlessTextureManager::allocateSlot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_freeSlots.empty()) {
        std::cerr << "No free texture slots available" << std::endl;
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

BindlessTextureHandle BindlessTextureManager::loadTexture(const std::string& path,
                                                            BindlessTextureType type,
                                                            bool generateMips) {
    // Check if already loaded
    auto it = m_pathToHandle.find(path);
    if (it != m_pathToHandle.end()) {
        return it->second;
    }

    // Load texture data
    std::vector<uint8_t> data;
    uint32_t width, height;
    VkFormat format;

    if (!loadTextureData(path, data, width, height, format)) {
        std::cerr << "Failed to load texture: " << path << std::endl;
        return getDefaultTexture(type);
    }

    // Create texture image
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t mipLevels;

    if (!createTextureImage(data.data(), width, height, format, generateMips,
                             image, memory, view, mipLevels)) {
        return getDefaultTexture(type);
    }

    // Allocate slot
    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        vkDestroyImageView(m_device, view, nullptr);
        vkDestroyImage(m_device, image, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
        return getDefaultTexture(type);
    }

    // Store texture info
    auto& tex = m_textures[slot];
    tex.image = image;
    tex.view = view;
    tex.memory = memory;
    tex.width = width;
    tex.height = height;
    tex.mipLevels = mipLevels;
    tex.format = format;
    tex.type = type;
    tex.name = path;

    BindlessTextureHandle handle{slot};
    m_pathToHandle[path] = handle;
    m_loadedCount++;
    m_totalMemoryUsage += width * height * 4;  // Approximate

    updateDescriptorSet();

    return handle;
}

BindlessTextureHandle BindlessTextureManager::loadTextureFromMemory(const void* data,
                                                                      uint32_t width, uint32_t height,
                                                                      VkFormat format,
                                                                      BindlessTextureType type,
                                                                      bool generateMips) {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t mipLevels;

    if (!createTextureImage(data, width, height, format, generateMips,
                             image, memory, view, mipLevels)) {
        return getDefaultTexture(type);
    }

    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        vkDestroyImageView(m_device, view, nullptr);
        vkDestroyImage(m_device, image, nullptr);
        vkFreeMemory(m_device, memory, nullptr);
        return getDefaultTexture(type);
    }

    auto& tex = m_textures[slot];
    tex.image = image;
    tex.view = view;
    tex.memory = memory;
    tex.width = width;
    tex.height = height;
    tex.mipLevels = mipLevels;
    tex.format = format;
    tex.type = type;
    tex.name = "memory_" + std::to_string(slot);

    BindlessTextureHandle handle{slot};
    m_loadedCount++;
    m_totalMemoryUsage += width * height * 4;

    updateDescriptorSet();

    return handle;
}

BindlessTextureHandle BindlessTextureManager::registerExternalTexture(VkImageView view,
                                                                        const std::string& name,
                                                                        BindlessTextureType type) {
    uint32_t slot = allocateSlot();
    if (slot == UINT32_MAX) {
        return BindlessTextureHandle{UINT32_MAX};
    }

    auto& tex = m_textures[slot];
    tex.view = view;
    tex.image = VK_NULL_HANDLE;  // External, don't destroy
    tex.memory = VK_NULL_HANDLE;
    tex.type = type;
    tex.name = name;

    BindlessTextureHandle handle{slot};
    m_nameToHandle[name] = handle;
    m_loadedCount++;

    updateDescriptorSet();

    return handle;
}

void BindlessTextureManager::unloadTexture(BindlessTextureHandle handle) {
    if (!handle.valid() || handle.index >= m_textures.size()) return;

    auto& tex = m_textures[handle.index];
    if (tex.persistent) return;  // Don't unload persistent textures

    // Remove from maps
    if (!tex.name.empty()) {
        m_nameToHandle.erase(tex.name);
        m_pathToHandle.erase(tex.name);
    }

    // Destroy resources
    if (tex.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, tex.view, nullptr);
    }
    if (tex.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, tex.image, nullptr);
    }
    if (tex.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, tex.memory, nullptr);
    }

    m_totalMemoryUsage -= tex.width * tex.height * 4;
    tex = BindlessTextureInfo{};  // Reset

    freeSlot(handle.index);
    m_loadedCount--;
}

const BindlessTextureInfo* BindlessTextureManager::getTextureInfo(BindlessTextureHandle handle) const {
    if (!handle.valid() || handle.index >= m_textures.size()) return nullptr;
    return &m_textures[handle.index];
}

BindlessTextureHandle BindlessTextureManager::getTextureByName(const std::string& name) const {
    auto it = m_nameToHandle.find(name);
    return it != m_nameToHandle.end() ? it->second : BindlessTextureHandle{UINT32_MAX};
}

BindlessTextureHandle BindlessTextureManager::getTextureByPath(const std::string& path) const {
    auto it = m_pathToHandle.find(path);
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
    imageInfos.reserve(m_loadedCount);

    std::vector<VkWriteDescriptorSet> writes;

    for (uint32_t i = 0; i < m_textures.size(); ++i) {
        if (m_textures[i].view != VK_NULL_HANDLE) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = m_defaultSampler;
            imageInfo.imageView = m_textures[i].view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos.push_back(imageInfo);

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSet;
            write.dstBinding = 0;
            write.dstArrayElement = i;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfos.back();
            writes.push_back(write);
        }
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

bool BindlessTextureManager::loadTextureData(const std::string& path, std::vector<uint8_t>& outData,
                                               uint32_t& width, uint32_t& height, VkFormat& format) {
    int w, h, channels;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);

    if (!pixels) {
        return false;
    }

    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    format = VK_FORMAT_R8G8B8A8_SRGB;

    size_t size = width * height * 4;
    outData.resize(size);
    memcpy(outData.data(), pixels, size);

    stbi_image_free(pixels);
    return true;
}

bool BindlessTextureManager::createTextureImage(const void* data, uint32_t width, uint32_t height,
                                                  VkFormat format, bool generateMips,
                                                  VkImage& outImage, VkDeviceMemory& outMemory,
                                                  VkImageView& outView, uint32_t& outMipLevels) {
    outMipLevels = generateMips ?
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;

    VkDeviceSize imageSize = width * height * 4;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memTypeIndex = i;
            break;
        }
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        return false;
    }

    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    // Copy data to staging buffer
    void* mapped;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, data, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingMemory);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = outMipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &outImage) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    // Allocate image memory
    VkMemoryRequirements imgMemReqs;
    vkGetImageMemoryRequirements(m_device, outImage, &imgMemReqs);

    uint32_t imgMemTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((imgMemReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            imgMemTypeIndex = i;
            break;
        }
    }

    VkMemoryAllocateInfo imgAllocInfo{};
    imgAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAllocInfo.allocationSize = imgMemReqs.size;
    imgAllocInfo.memoryTypeIndex = imgMemTypeIndex;

    if (vkAllocateMemory(m_device, &imgAllocInfo, nullptr, &outMemory) != VK_SUCCESS) {
        vkDestroyImage(m_device, outImage, nullptr);
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return false;
    }

    vkBindImageMemory(m_device, outImage, outMemory, 0);

    // Transition and copy
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to transfer dst
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = outImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = outMipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Generate mipmaps or transition to shader read
    if (generateMips && outMipLevels > 1) {
        generateMipmaps(cmd, outImage, width, height, outMipLevels);
    } else {
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = outMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &outView) != VK_SUCCESS) {
        vkDestroyImage(m_device, outImage, nullptr);
        vkFreeMemory(m_device, outMemory, nullptr);
        return false;
    }

    return true;
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
