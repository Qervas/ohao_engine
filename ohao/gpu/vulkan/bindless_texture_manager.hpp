#pragma once

#include "core/concepts.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "gpu/vulkan/submitted_work.hpp"

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>
#include <span>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <compare>
#include <glm/glm.hpp>

namespace ohao {

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
    GpuAllocation allocation;   // empty: a view registered from outside
    uint32_t width{0};
    uint32_t height{0};
    uint32_t mipLevels{1};
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
    BindlessTextureType type{BindlessTextureType::Custom};
    std::string name;
    bool persistent{false};  // If true, not unloaded during streaming
};

// A texture named by what it is made of: the hash of its pixels, and how they are uploaded. Two users of the same
// content get one table entry (BindlessTextureManager::contentTexture).
struct TextureContentKey {
    uint64_t hi{0}, lo{0};                         // the pixels' content hash (asset/content_hash.hpp)
    VkFormat format{VK_FORMAT_R8G8B8A8_SRGB};
    BindlessTextureType type{BindlessTextureType::Custom};
    [[nodiscard]] constexpr auto operator<=>(const TextureContentKey&) const noexcept = default;
};

// A texture's pixels and whatever keeps them alive (a cooked file's mapping): level 0 as RGBA8 (the other levels
// are then made by blits), or every level in the texture's format (a cooked texture's: BC7, BC5), level 0 first.
struct TexturePixels {
    std::span<const uint8_t> rgba8;
    uint32_t width{0}, height{0};
    std::shared_ptr<const void> keep;
    std::vector<std::span<const uint8_t>> levels;
    std::array<uint8_t, 4> channelMin{255, 255, 255, 255};   // each channel's smallest value, where the source knows it
};

struct ContentTexture {
    BindlessTextureHandle handle;
    std::array<uint8_t, 4> channelMin{255, 255, 255, 255};   // TexturePixels::channelMin
};

// Manages bindless texture array with dynamic updates
class BindlessTextureManager {
public:
    // The table's binding in its set; every shader that reads it declares it there
    // (render/shader_bindings/shader_bindings.cpp holds them to it).
    static constexpr uint32_t kBinding = 0;

    BindlessTextureManager() = default;
    ~BindlessTextureManager();

    // Initialization
    [[nodiscard]] bool initialize(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
                    uint32_t maxTextures = 4096, uint32_t graphicsQueueFamily = 0,
                    VkQueue graphicsQueue = VK_NULL_HANDLE);
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

    // The texture of this content, made when nobody holds it: `open` is called only then. Each call is one hold,
    // given back with releaseContent. Its table
    // index is good at once; its pixels reach the GPU at the next flushUploads(), which the caller owes before a
    // frame samples it (instantiate flushes once per run). A texture that cannot be opened or made is the type's
    // default (and is asked for again next time).
    [[nodiscard]] ContentTexture contentTexture(const TextureContentKey& key, const std::function<TexturePixels()>& open);
    [[nodiscard]] std::size_t contentTextureCount() const noexcept { return m_byContent.size(); }
    // Gives back one hold of a contentTexture (a handle that is not one, a default, is ignored). With the last hold
    // gone the texture leaves the table; its image is destroyed kRetireFrames frames later (onFrameRetired), when
    // no frame in flight can still sample it. No device wait.
    void releaseContent(BindlessTextureHandle handle);
    static constexpr uint32_t kRetireFrames = 3;   // at least the renderer's frames in flight
    void onFrameRetired();                         // once per frame, after the frame slot's fence wait
    // Submits the uploads of the textures still waiting (a load uploads as it goes, one staging buffer and one
    // submit per 64 MB of pixels) and writes the table once. It does not wait for the GPU: a frame submitted
    // afterwards samples them uploaded.
    void flushUploads();

    /// Overwrite an existing texture's texels in place (same WxH). No new slot / no unload.
    /// Used by Diff dense-map FD loop for fast beauty SoT updates.
    [[nodiscard]] bool updateTextureFromMemory(BindlessTextureHandle handle,
                                               std::span<const uint8_t> data,
                                               uint32_t width, uint32_t height);

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
    // One past the highest slot holding a view. Bindless indices are slots, so a
    // consumer building its own descriptor array binds [0, usedSlotEnd()).
    [[nodiscard]] uint32_t usedSlotEnd() const noexcept {
        for (uint32_t i = static_cast<uint32_t>(m_textures.size()); i > 0; --i)
            if (m_textures[i - 1].view != VK_NULL_HANDLE) return i;
        return 0;
    }
    // The bytes of the loaded textures' pixels as the GPU holds them (compressed where they are), mips included
    // (not the views registered from outside).
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

    // A texture's image, view and table slot, its pixels queued for flushUploads(). UINT32_MAX: not made.
    [[nodiscard]] uint32_t addTexture(TexturePixels pixels, VkFormat format, BindlessTextureType type, bool mips,
                                      std::string name);
    void recordUpload(VkCommandBuffer cmd, VkBuffer staging, VkDeviceSize offset, const BindlessTextureInfo& tex,
                      bool everyLevel);

    void generateMipmaps(VkCommandBuffer cmd, VkImage image,
                          uint32_t width, uint32_t height, uint32_t mipLevels);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    GpuAllocator m_allocator;   // the textures' images and the staging buffers
    gpu::SubmittedWork m_work;  // the uploads the GPU has not finished

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
    struct ContentEntry {
        ContentTexture texture;
        uint32_t holds{0};
    };
    std::map<TextureContentKey, ContentEntry> m_byContent;
    std::unordered_map<uint32_t, TextureContentKey> m_contentOfSlot;
    struct RetiredTexture {
        uint64_t at;     // the frame it may be destroyed in
        uint32_t slot;
    };
    std::vector<RetiredTexture> m_retired;
    uint64_t m_frame{0};
    void destroyTexture(uint32_t slot);   // its view and image now, its slot free, the table entry the default's
    struct PendingUpload {
        uint32_t slot;
        TexturePixels pixels;
        std::size_t bytes;   // of every level it holds
    };
    std::vector<PendingUpload> m_pending;
    static constexpr VkDeviceSize kStaging = 64ull << 20;   // one staging buffer, one submit
    VkDeviceSize m_pendingBytes{0};
    bool m_tableStale{false};   // uploaded since the table was last written
    void uploadPending();

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
