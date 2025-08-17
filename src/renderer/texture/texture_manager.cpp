#include "texture_manager.hpp"
#include "renderer/rhi/vk/ohao_vk_device.hpp"
#include "renderer/rhi/vk/ohao_vk_image.hpp"
#include "ui/components/console_widget.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <iostream>
#include <algorithm>

namespace ohao {

TextureManager::~TextureManager() {
    cleanup();
}

bool TextureManager::initialize(OhaoVkDevice* devicePtr) {
    device = devicePtr;
    if (!device) {
        OHAO_LOG_ERROR("TextureManager: Invalid device provided");
        return false;
    }
    
    // Create default textures
    createDefaultTextures();
    
    OHAO_LOG("TextureManager initialized successfully");
    return true;
}

void TextureManager::cleanup() {
    if (!device) return;
    
    // Wait for device to be idle before cleanup
    device->waitIdle();
    
    // Clean up all textures
    for (auto& [path, textureData] : textures) {
        if (textureData->sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device->getDevice(), textureData->sampler, nullptr);
        }
        if (textureData->imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device->getDevice(), textureData->imageView, nullptr);
        }
        // OhaoVkImage will clean itself up
    }
    
    textures.clear();
    device = nullptr;
}

bool TextureManager::loadTexture(const std::string& path) {
    // Check if already loaded
    if (hasTexture(path)) {
        return true;
    }
    
    // Load image data using stb_image
    int width, height, channels;
    stbi_set_flip_vertically_on_load(true); // Flip for Vulkan coordinate system
    
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!data) {
        OHAO_LOG_ERROR("Failed to load texture: " + path);
        return false;
    }
    
    // Force to 4 channels (RGBA)
    channels = 4;
    
    bool success = createTextureFromData(path, data, width, height, channels);
    
    // Free image data
    stbi_image_free(data);
    
    return success;
}

TextureData* TextureManager::getTexture(const std::string& path) {
    auto it = textures.find(path);
    if (it != textures.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool TextureManager::hasTexture(const std::string& path) const {
    return textures.find(path) != textures.end();
}

void TextureManager::createDefaultTextures() {
    // White texture (default albedo)
    createDefaultTexture("default_white", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    
    // Normal map (default normal - pointing up in tangent space)
    createDefaultTexture("default_normal", glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
    
    // Black texture (default metallic)
    createDefaultTexture("default_metallic", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    
    // Gray texture (default roughness)
    createDefaultTexture("default_roughness", glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
    
    // White texture (default AO)
    createDefaultTexture("default_ao", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    
    // Black texture (default emissive)
    createDefaultTexture("default_emissive", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
}

bool TextureManager::createTextureFromData(const std::string& path, unsigned char* data, int width, int height, int channels) {
    try {
        auto textureData = std::make_unique<TextureData>();
        textureData->width = static_cast<uint32_t>(width);
        textureData->height = static_cast<uint32_t>(height);
        textureData->channels = static_cast<uint32_t>(channels);
        textureData->path = path;
        
        // Calculate image size
        VkDeviceSize imageSize = width * height * 4; // Force RGBA
        
        // Create VkImage
        textureData->image = std::make_unique<OhaoVkImage>();
        if (!textureData->image->createTextureImage(device, width, height, data)) {
            OHAO_LOG_ERROR("Failed to create texture image for: " + path);
            return false;
        }
        
        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = textureData->image->getImage();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        if (vkCreateImageView(device->getDevice(), &viewInfo, nullptr, &textureData->imageView) != VK_SUCCESS) {
            OHAO_LOG_ERROR("Failed to create texture image view for: " + path);
            return false;
        }
        
        // Create sampler
        textureData->sampler = createTextureSampler();
        if (textureData->sampler == VK_NULL_HANDLE) {
            OHAO_LOG_ERROR("Failed to create texture sampler for: " + path);
            return false;
        }
        
        // Store the texture
        textures[path] = std::move(textureData);
        
        OHAO_LOG("Successfully loaded texture: " + path + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
        return true;
        
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Exception while creating texture from data: " + std::string(e.what()));
        return false;
    }
}

void TextureManager::createDefaultTexture(const std::string& name, const glm::vec4& color) {
    // Create 1x1 pixel texture with the specified color
    unsigned char data[4] = {
        static_cast<unsigned char>(color.r * 255),
        static_cast<unsigned char>(color.g * 255),
        static_cast<unsigned char>(color.b * 255),
        static_cast<unsigned char>(color.a * 255)
    };
    
    createTextureFromData(name, data, 1, 1, 4);
}

VkSampler TextureManager::createTextureSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    
    // Get max anisotropy from device
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device->getPhysicalDevice()->getDevice(), &properties);
    samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
    
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    VkSampler sampler;
    if (vkCreateSampler(device->getDevice(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        OHAO_LOG_ERROR("Failed to create texture sampler");
        return VK_NULL_HANDLE;
    }
    
    return sampler;
}

} // namespace ohao