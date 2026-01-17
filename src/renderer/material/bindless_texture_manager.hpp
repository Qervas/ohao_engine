#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <mutex>
#include <glm/glm.hpp>

namespace ohao {

// Forward declarations
class GpuAllocator;

// Texture handle for bindless access
struct BindlessTextureHandle {
    uint32_t index{UINT32_MAX};
    bool valid() const { return index != UINT32_MAX; }

    bool operator==(const BindlessTextureHandle& other) const { return index == other.index; }
    bool operator!=(const BindlessTextureHandle& other) const { return index != other.index; }
};

// Texture type enum
enum class BindlessTextureType : uint32_t {
    Albedo = 0,
    Normal = 1,
    Roughness = 2,
    Metallic = 3,
    AO = 4,
    Emissive = 5,
    Height = 6,
    Opacity = 7,
    Cubemap = 8,
    IBLDiffuse = 9,
    IBLSpecular = 10,
    BRDF_LUT = 11,
    Custom = 255
};

// Texture info stored with each bindless texture
struct BindlessTextureInfo {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t mipLevels{1};
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
    BindlessTextureType type{BindlessTextureType::Custom};
    std::string name;
    bool persistent{false};  // If true, not unloaded during streaming
};

// Manages bindless texture array with dynamic updates
class BindlessTextureManager {
public:
    BindlessTextureManager() = default;
    ~BindlessTextureManager();

    // Initialization
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    GpuAllocator* allocator, uint32_t maxTextures = 4096,
                    uint32_t graphicsQueueFamily = 0, VkQueue graphicsQueue = VK_NULL_HANDLE);
    void cleanup();

    // Texture loading
    BindlessTextureHandle loadTexture(const std::string& path,
                                       BindlessTextureType type = BindlessTextureType::Custom,
                                       bool generateMips = true);

    BindlessTextureHandle loadTextureFromMemory(const void* data, uint32_t width, uint32_t height,
                                                  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
                                                  BindlessTextureType type = BindlessTextureType::Custom,
                                                  bool generateMips = true);

    BindlessTextureHandle registerExternalTexture(VkImageView view, const std::string& name,
                                                    BindlessTextureType type = BindlessTextureType::Custom);

    // Unload texture (marks slot as free)
    void unloadTexture(BindlessTextureHandle handle);

    // Query texture info
    const BindlessTextureInfo* getTextureInfo(BindlessTextureHandle handle) const;
    BindlessTextureHandle getTextureByName(const std::string& name) const;
    BindlessTextureHandle getTextureByPath(const std::string& path) const;

    // Mark texture as persistent (won't be unloaded during streaming)
    void setTexturePersistent(BindlessTextureHandle handle, bool persistent);

    // Descriptor set for shader binding
    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }

    // Update descriptor set (call after loading/unloading textures)
    void updateDescriptorSet();

    // Default textures
    BindlessTextureHandle getDefaultWhiteTexture() const { return m_defaultWhite; }
    BindlessTextureHandle getDefaultBlackTexture() const { return m_defaultBlack; }
    BindlessTextureHandle getDefaultNormalTexture() const { return m_defaultNormal; }
    BindlessTextureHandle getDefaultTexture(BindlessTextureType type) const;

    // Stats
    uint32_t getLoadedTextureCount() const { return m_loadedCount; }
    uint32_t getMaxTextures() const { return m_maxTextures; }
    size_t getTotalMemoryUsage() const { return m_totalMemoryUsage; }

private:
    bool createDescriptorResources();
    bool createDefaultTextures();
    bool createSolidColorTexture(uint32_t color, BindlessTextureHandle& outHandle,
                                  const std::string& name);
    bool createDefaultNormalTexture();

    uint32_t allocateSlot();
    void freeSlot(uint32_t slot);

    bool loadTextureData(const std::string& path, std::vector<uint8_t>& outData,
                          uint32_t& width, uint32_t& height, VkFormat& format);

    bool createTextureImage(const void* data, uint32_t width, uint32_t height,
                             VkFormat format, bool generateMips,
                             VkImage& outImage, VkDeviceMemory& outMemory,
                             VkImageView& outView, uint32_t& outMipLevels);

    void generateMipmaps(VkCommandBuffer cmd, VkImage image,
                          uint32_t width, uint32_t height, uint32_t mipLevels);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};

    // Descriptor resources
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkSampler m_defaultSampler{VK_NULL_HANDLE};

    // Texture storage
    std::vector<BindlessTextureInfo> m_textures;
    std::vector<uint32_t> m_freeSlots;
    std::unordered_map<std::string, BindlessTextureHandle> m_pathToHandle;
    std::unordered_map<std::string, BindlessTextureHandle> m_nameToHandle;

    // Default textures
    BindlessTextureHandle m_defaultWhite;
    BindlessTextureHandle m_defaultBlack;
    BindlessTextureHandle m_defaultNormal;

    uint32_t m_maxTextures{4096};
    uint32_t m_loadedCount{0};
    size_t m_totalMemoryUsage{0};

    std::mutex m_mutex;

    // Command pool for texture uploads
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
};

} // namespace ohao
