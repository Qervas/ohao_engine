#pragma once

/**
 * Small Vulkan C++20 helpers (art bar for gpu/).
 *
 * Prefer these over raw VkResult checks and bare void* mapped pointers.
 * Does not wrap the Vulkan API — stays thin and header-only.
 */

#include "core/concepts.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vulkan/vulkan.h>

namespace ohao {

// ─── VkResult ───────────────────────────────────────────────────────────────

[[nodiscard]] constexpr bool vk_ok(VkResult r) noexcept {
    return r == VK_SUCCESS;
}

[[nodiscard]] constexpr bool vk_failed(VkResult r) noexcept {
    return r != VK_SUCCESS;
}

[[nodiscard]] constexpr std::string_view vk_result_name(VkResult r) noexcept {
    switch (r) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
        default:                                return "VK_RESULT_OTHER";
    }
}

[[nodiscard]] inline VoidResult<> vk_check(VkResult r, std::string_view context) {
    if (vk_ok(r)) return VoidResult<>::ok();
    return VoidResult<>::err(std::string(context) + ": " + std::string(vk_result_name(r)));
}

// ─── Mapped memory as span ──────────────────────────────────────────────────

template<GpuPod T>
[[nodiscard]] constexpr std::span<T> as_mapped_span(void* mapped, std::size_t count) noexcept {
    return std::span<T>{static_cast<T*>(mapped), count};
}

template<GpuPod T>
[[nodiscard]] constexpr std::span<const T> as_mapped_span(const void* mapped, std::size_t count) noexcept {
    return std::span<const T>{static_cast<const T*>(mapped), count};
}

/// Byte view of any POD range (upload staging).
template<GpuPod T>
[[nodiscard]] constexpr std::span<const std::byte> as_bytes(std::span<const T> s) noexcept {
    return std::as_bytes(s);
}

template<GpuPod T>
[[nodiscard]] constexpr std::size_t byte_size(std::span<const T> s) noexcept {
    return s.size_bytes();
}

// ─── Handle validity ────────────────────────────────────────────────────────

[[nodiscard]] constexpr bool vk_handle_valid(VkBuffer b) noexcept { return b != VK_NULL_HANDLE; }
[[nodiscard]] constexpr bool vk_handle_valid(VkImage i) noexcept { return i != VK_NULL_HANDLE; }
[[nodiscard]] constexpr bool vk_handle_valid(VkDevice d) noexcept { return d != VK_NULL_HANDLE; }
[[nodiscard]] constexpr bool vk_handle_valid(VkImageView v) noexcept { return v != VK_NULL_HANDLE; }

// ─── Extent / size helpers ──────────────────────────────────────────────────

[[nodiscard]] constexpr VkExtent2D make_extent2d(std::uint32_t w, std::uint32_t h) noexcept {
    return VkExtent2D{.width = w, .height = h};
}

[[nodiscard]] constexpr std::uint32_t pixel_count(VkExtent2D e) noexcept {
    return e.width * e.height;
}

[[nodiscard]] constexpr std::uint32_t mip_levels_for(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    std::uint32_t dim = width > height ? width : height;
    while (dim > 1u) {
        dim >>= 1u;
        ++levels;
    }
    return levels;
}

} // namespace ohao
