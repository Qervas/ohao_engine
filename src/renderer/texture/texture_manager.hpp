#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <memory>

namespace ohao {

class OhaoVkDevice;
class OhaoVkImage;

struct TextureData {
    std::unique_ptr<OhaoVkImage> image;
    VkImageView imageView{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t channels{0};
    std::string path;
};

class TextureManager {
public:
    TextureManager() = default;
    ~TextureManager();

    bool initialize(OhaoVkDevice* device);
    void cleanup();

    // Load texture from file
    bool loadTexture(const std::string& path);
    
    // Get texture data
    TextureData* getTexture(const std::string& path);
    
    // Check if texture exists
    bool hasTexture(const std::string& path) const;
    
    // Create default textures (white, normal, etc.)
    void createDefaultTextures();
    
    // Get default texture paths
    static std::string getDefaultAlbedoTexture() { return "default_white"; }
    static std::string getDefaultNormalTexture() { return "default_normal"; }
    static std::string getDefaultMetallicTexture() { return "default_metallic"; }
    static std::string getDefaultRoughnessTexture() { return "default_roughness"; }
    
    // Get all loaded textures (for UI)
    const std::unordered_map<std::string, std::unique_ptr<TextureData>>& getAllTextures() const { return textures; }

private:
    OhaoVkDevice* device{nullptr};
    std::unordered_map<std::string, std::unique_ptr<TextureData>> textures;
    
    // Helper methods
    bool createTextureFromData(const std::string& path, unsigned char* data, int width, int height, int channels);
    void createDefaultTexture(const std::string& name, const glm::vec4& color);
    VkSampler createTextureSampler();
};

} // namespace ohao