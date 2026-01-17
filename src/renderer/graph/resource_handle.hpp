#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <limits>

namespace ohao {

/**
 * Handle types for render graph resources.
 * These are indices into resource arrays, not Vulkan handles.
 * They become valid Vulkan resources after graph compilation.
 */
struct TextureHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};

    bool isValid() const { return index != std::numeric_limits<uint32_t>::max(); }
    bool operator==(const TextureHandle& other) const { return index == other.index; }
    bool operator!=(const TextureHandle& other) const { return index != other.index; }

    static TextureHandle invalid() { return {}; }
};

struct BufferHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};

    bool isValid() const { return index != std::numeric_limits<uint32_t>::max(); }
    bool operator==(const BufferHandle& other) const { return index == other.index; }
    bool operator!=(const BufferHandle& other) const { return index != other.index; }

    static BufferHandle invalid() { return {}; }
};

/**
 * Texture usage flags for determining required barriers
 */
enum class TextureUsage : uint32_t {
    None            = 0,
    ColorAttachment = 1 << 0,
    DepthAttachment = 1 << 1,
    ShaderRead      = 1 << 2,
    ShaderWrite     = 1 << 3,
    TransferSrc     = 1 << 4,
    TransferDst     = 1 << 5,
    Present         = 1 << 6,
    Storage         = 1 << 7
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline TextureUsage operator&(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(TextureUsage usage, TextureUsage flag) {
    return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * Buffer usage flags for determining required barriers
 */
enum class BufferUsage : uint32_t {
    None          = 0,
    VertexBuffer  = 1 << 0,
    IndexBuffer   = 1 << 1,
    UniformBuffer = 1 << 2,
    StorageBuffer = 1 << 3,
    IndirectBuffer = 1 << 4,
    TransferSrc   = 1 << 5,
    TransferDst   = 1 << 6
};

inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline BufferUsage operator&(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(BufferUsage usage, BufferUsage flag) {
    return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0;
}

/**
 * Texture description for graph resource creation
 */
struct TextureDesc {
    std::string name;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
    uint32_t mipLevels{1};
    uint32_t arrayLayers{1};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    TextureUsage usage{TextureUsage::None};

    // Transient textures can have their memory aliased with other transient resources
    bool isTransient{true};

    // External textures are managed outside the graph (e.g., swapchain images)
    bool isExternal{false};

    static TextureDesc colorTarget(const std::string& name, uint32_t w, uint32_t h,
                                    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB) {
        return {name, w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::ColorAttachment | TextureUsage::ShaderRead, true, false};
    }

    static TextureDesc depthTarget(const std::string& name, uint32_t w, uint32_t h,
                                    VkFormat format = VK_FORMAT_D32_SFLOAT) {
        return {name, w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::DepthAttachment | TextureUsage::ShaderRead, true, false};
    }

    static TextureDesc hdrTarget(const std::string& name, uint32_t w, uint32_t h) {
        return {name, w, h, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::ColorAttachment | TextureUsage::ShaderRead, true, false};
    }

    static TextureDesc shadowMap(const std::string& name, uint32_t size) {
        return {name, size, size, 1, 1, 1, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::DepthAttachment | TextureUsage::ShaderRead, true, false};
    }

    static TextureDesc gbuffer(const std::string& name, uint32_t w, uint32_t h, VkFormat format) {
        return {name, w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::ColorAttachment | TextureUsage::ShaderRead, true, false};
    }
};

/**
 * Buffer description for graph resource creation
 */
struct BufferDesc {
    std::string name;
    VkDeviceSize size{0};
    BufferUsage usage{BufferUsage::None};

    // Transient buffers can have their memory aliased
    bool isTransient{true};

    static BufferDesc uniform(const std::string& name, VkDeviceSize size) {
        return {name, size, BufferUsage::UniformBuffer, true};
    }

    static BufferDesc storage(const std::string& name, VkDeviceSize size) {
        return {name, size, BufferUsage::StorageBuffer, true};
    }

    static BufferDesc indirect(const std::string& name, VkDeviceSize size) {
        return {name, size, BufferUsage::IndirectBuffer, true};
    }
};

/**
 * Resource access information for a pass
 */
struct ResourceAccess {
    TextureHandle texture;
    BufferHandle buffer;
    TextureUsage textureUsage{TextureUsage::None};
    BufferUsage bufferUsage{BufferUsage::None};

    // For barrier generation
    VkPipelineStageFlags stageMask{0};
    VkAccessFlags accessMask{0};
    VkImageLayout imageLayout{VK_IMAGE_LAYOUT_UNDEFINED};

    bool isRead() const {
        return hasFlag(textureUsage, TextureUsage::ShaderRead) ||
               hasFlag(bufferUsage, BufferUsage::UniformBuffer) ||
               hasFlag(bufferUsage, BufferUsage::VertexBuffer) ||
               hasFlag(bufferUsage, BufferUsage::IndexBuffer);
    }

    bool isWrite() const {
        return hasFlag(textureUsage, TextureUsage::ColorAttachment) ||
               hasFlag(textureUsage, TextureUsage::DepthAttachment) ||
               hasFlag(textureUsage, TextureUsage::ShaderWrite) ||
               hasFlag(bufferUsage, BufferUsage::StorageBuffer);
    }
};

} // namespace ohao

// Hash functions for handles
namespace std {
    template<>
    struct hash<ohao::TextureHandle> {
        size_t operator()(const ohao::TextureHandle& h) const {
            return hash<uint32_t>()(h.index);
        }
    };

    template<>
    struct hash<ohao::BufferHandle> {
        size_t operator()(const ohao::BufferHandle& h) const {
            return hash<uint32_t>()(h.index);
        }
    };
}
