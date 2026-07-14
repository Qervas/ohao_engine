#pragma once

#include "core/concepts.hpp"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <limits>
#include <compare>

namespace ohao {

/**
 * Handle types for render graph resources.
 * These are indices into resource arrays, not Vulkan handles.
 * They become valid Vulkan resources after graph compilation.
 */
struct TextureHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return index != std::numeric_limits<uint32_t>::max();
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return isValid(); }
    [[nodiscard]] constexpr auto operator<=>(const TextureHandle&) const noexcept = default;

    [[nodiscard]] static constexpr TextureHandle invalid() noexcept { return {}; }
};

struct BufferHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return index != std::numeric_limits<uint32_t>::max();
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return isValid(); }
    [[nodiscard]] constexpr auto operator<=>(const BufferHandle&) const noexcept = default;

    [[nodiscard]] static constexpr BufferHandle invalid() noexcept { return {}; }
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

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(to_underlying(a) | to_underlying(b));
}
[[nodiscard]] constexpr TextureUsage operator&(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(to_underlying(a) & to_underlying(b));
}
constexpr TextureUsage& operator|=(TextureUsage& a, TextureUsage b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool hasFlag(TextureUsage usage, TextureUsage flag) noexcept {
    return (to_underlying(usage) & to_underlying(flag)) != 0;
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

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(to_underlying(a) | to_underlying(b));
}
[[nodiscard]] constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(to_underlying(a) & to_underlying(b));
}
constexpr BufferUsage& operator|=(BufferUsage& a, BufferUsage b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool hasFlag(BufferUsage usage, BufferUsage flag) noexcept {
    return (to_underlying(usage) & to_underlying(flag)) != 0;
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

    [[nodiscard]] static TextureDesc colorTarget(std::string_view name, uint32_t w, uint32_t h,
                                    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB) {
        return {std::string(name), w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::ColorAttachment | TextureUsage::ShaderRead, true, false};
    }

    [[nodiscard]] static TextureDesc depthTarget(std::string_view name, uint32_t w, uint32_t h,
                                    VkFormat format = VK_FORMAT_D32_SFLOAT) {
        return {std::string(name), w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::DepthAttachment | TextureUsage::ShaderRead, true, false};
    }

    [[nodiscard]] static TextureDesc hdrTarget(std::string_view name, uint32_t w, uint32_t h) {
        return {std::string(name), w, h, 1, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::ColorAttachment | TextureUsage::ShaderRead, true, false};
    }

    [[nodiscard]] static TextureDesc shadowMap(std::string_view name, uint32_t size) {
        return {std::string(name), size, size, 1, 1, 1, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                TextureUsage::DepthAttachment | TextureUsage::ShaderRead, true, false};
    }

    [[nodiscard]] static TextureDesc gbuffer(std::string_view name, uint32_t w, uint32_t h, VkFormat format) {
        return {std::string(name), w, h, 1, 1, 1, format, VK_SAMPLE_COUNT_1_BIT,
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

    [[nodiscard]] static BufferDesc uniform(std::string_view name, VkDeviceSize size) {
        return {std::string(name), size, BufferUsage::UniformBuffer, true};
    }

    [[nodiscard]] static BufferDesc storage(std::string_view name, VkDeviceSize size) {
        return {std::string(name), size, BufferUsage::StorageBuffer, true};
    }

    [[nodiscard]] static BufferDesc indirect(std::string_view name, VkDeviceSize size) {
        return {std::string(name), size, BufferUsage::IndirectBuffer, true};
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

    [[nodiscard]] bool isRead() const noexcept {
        return hasFlag(textureUsage, TextureUsage::ShaderRead) ||
               hasFlag(bufferUsage, BufferUsage::UniformBuffer) ||
               hasFlag(bufferUsage, BufferUsage::VertexBuffer) ||
               hasFlag(bufferUsage, BufferUsage::IndexBuffer);
    }

    [[nodiscard]] bool isWrite() const noexcept {
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
