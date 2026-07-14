#pragma once

#include "core/concepts.hpp"

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <span>
#include <memory>
#include <mutex>
#include <cstdint>
#include <compare>
#include <glm/glm.hpp>

namespace ohao {

// Forward declarations
class GpuAllocator;

// Texture handle for bindless access
struct BindlessTextureHandle {
    uint32_t index{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != UINT32_MAX; }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return valid(); }

    [[nodiscard]] constexpr auto operator<=>(const BindlessTextureHandle&) const noexcept = default;
};

inline constexpr BindlessTextureHandle kInvalidBindlessTexture{};

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
    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    GpuAllocator* allocator, uint32_t maxTextures = 4096,
                    uint32_t graphicsQueueFamily = 0, VkQueue graphicsQueue = VK_NULL_HANDLE);
    void cleanup();

    // Texture loading
    [[nodiscard]] BindlessTextureHandle loadTexture(std::string_view path,
                                       BindlessTextureType type = BindlessTextureType::Custom,
                                       bool generateMips = true);

    [[nodiscard]] BindlessTextureHandle loadTextureFromMemory(std::span<const uint8_t> data,
                                                  uint32_t width, uint32_t height,
                                                  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB,
                                                  BindlessTextureType type = BindlessTextureType::Custom,
                                                  bool generateMips = true);

    [[nodiscard]] BindlessTextureHandle registerExternalTexture(VkImageView view, std::string_view name,
                                                    BindlessTextureType type = BindlessTextureType::Custom);

    // Unload texture (marks slot as free)
    void unloadTexture(BindlessTextureHandle handle);

    // Query texture info
    [[nodiscard]] const BindlessTextureInfo* getTextureInfo(BindlessTextureHandle handle) const;
    // Unified lookup (checks name first, then path)
    [[nodiscard]] BindlessTextureHandle findTexture(std::string_view key) const;

    [[nodiscard]] BindlessTextureHandle getTextureByName(std::string_view name) const;
    [[nodiscard]] BindlessTextureHandle getTextureByPath(std::string_view path) const;

    // Register a name/path for an existing handle (so getTextureByPath finds it)
    void registerName(BindlessTextureHandle handle, std::string_view name) {
        if (handle.valid()) {
            std::string key(name);
            m_pathToHandle[key] = handle;
            m_nameToHandle[key] = handle;
        }
    }

    // Mark texture as persistent (won't be unloaded during streaming)
    void setTexturePersistent(BindlessTextureHandle handle, bool persistent);

    // Quick accessors for a specific texture's view/sampler
    [[nodiscard]] VkImageView getImageView(BindlessTextureHandle handle) const {
        const auto* info = getTextureInfo(handle);
        return info ? info->view : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkSampler getSampler(BindlessTextureHandle /*handle*/) const { return m_defaultSampler; }
    [[nodiscard]] VkSampler getDefaultSampler() const { return m_defaultSampler; }

    // Descriptor set for shader binding
    [[nodiscard]] VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    [[nodiscard]] VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }

    // Update descriptor set (call after loading/unloading textures)
    void updateDescriptorSet();

    // Default textures
    [[nodiscard]] BindlessTextureHandle getDefaultWhiteTexture() const { return m_defaultWhite; }
    [[nodiscard]] BindlessTextureHandle getDefaultBlackTexture() const { return m_defaultBlack; }
    [[nodiscard]] BindlessTextureHandle getDefaultNormalTexture() const { return m_defaultNormal; }
    [[nodiscard]] BindlessTextureHandle getDefaultTexture(BindlessTextureType type) const;

    // Stats
    [[nodiscard]] uint32_t getLoadedTextureCount() const noexcept { return m_loadedCount; }
    [[nodiscard]] uint32_t getMaxTextures() const noexcept { return m_maxTextures; }
    [[nodiscard]] size_t getTotalMemoryUsage() const noexcept { return m_totalMemoryUsage; }
    [[nodiscard]] float fillRatio() const noexcept {
        return m_maxTextures == 0 ? 0.f
                                  : static_cast<float>(m_loadedCount) / static_cast<float>(m_maxTextures);
    }
    [[nodiscard]] bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

private:
    [[nodiscard]] bool createDescriptorResources();
    [[nodiscard]] bool createDefaultTextures();
    [[nodiscard]] bool createSolidColorTexture(uint32_t color, BindlessTextureHandle& outHandle,
                                  std::string_view name);
    [[nodiscard]] bool createDefaultNormalTexture();

    uint32_t allocateSlot();
    void freeSlot(uint32_t slot);

    [[nodiscard]] bool loadTextureData(std::string_view path, BindlessTextureType type, std::vector<uint8_t>& outData,
                          uint32_t& width, uint32_t& height, VkFormat& format);

    [[nodiscard]] bool createTextureImage(std::span<const uint8_t> data, uint32_t width, uint32_t height,
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
