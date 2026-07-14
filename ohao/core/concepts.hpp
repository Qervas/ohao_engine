#pragma once

/**
 * OHAO C++20 concepts — core vocabulary.
 *
 * Style template for the rest of the engine:
 *   - Prefer concepts over enable_if / static_assert(is_base_of)
 *   - Prefer span/views at buffer boundaries
 *   - Keep runtime plugins (virtual) documented with *Like concepts
 */

#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace ohao {

class Component;
class Command;

// ─── Entity / component ─────────────────────────────────────────────────────

template<typename T>
concept ComponentType = std::derived_from<T, Component>;

template<typename T>
concept EnumType = std::is_enum_v<T>;

// ─── Callables ──────────────────────────────────────────────────────────────

template<typename F>
concept NullaryCallable = std::invocable<F> && std::same_as<std::invoke_result_t<F>, void>;

template<typename F, typename... Args>
concept Predicate = std::predicate<F, Args...>;

// ─── Command surface (virtual Command + static helpers) ─────────────────────

template<typename T>
concept CommandLike = requires(T& t, const T& ct) {
    { t.execute() } -> std::same_as<void>;
    { t.undo() } -> std::same_as<void>;
    { ct.getDescription() } -> std::convertible_to<std::string>;
};

// ─── GPU / buffer views ─────────────────────────────────────────────────────

template<typename T>
concept GpuPod = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template<typename R, typename T>
concept ContiguousRangeOf =
    std::ranges::contiguous_range<R> &&
    std::ranges::sized_range<R> &&
    std::same_as<std::remove_cvref_t<std::ranges::range_value_t<R>>, T>;

template<typename T, ContiguousRangeOf<T> R>
[[nodiscard]] constexpr std::span<T> as_span(R&& r) noexcept {
    return std::span<T>{std::ranges::data(r), std::ranges::size(r)};
}

template<typename T, ContiguousRangeOf<T> R>
[[nodiscard]] constexpr std::span<const T> as_const_span(const R& r) noexcept {
    return std::span<const T>{std::ranges::data(r), std::ranges::size(r)};
}

template<typename T>
[[nodiscard]] constexpr bool span_covers_image(std::span<const T> s,
                                               std::size_t width,
                                               std::size_t height,
                                               std::size_t channels) noexcept {
    const auto need = width * height * channels;
    return need == 0 || s.size() >= need;
}

template<typename T, std::size_t ExpectedBytes>
concept GpuLayoutBytes = GpuPod<T> && sizeof(T) == ExpectedBytes;

#define OHAO_ASSERT_GPU_LAYOUT(Type, Bytes)                                    \
    static_assert(::ohao::GpuPod<Type>,                                        \
                  #Type " must be trivially copyable standard-layout for GPU"); \
    static_assert(sizeof(Type) == (Bytes),                                     \
                  #Type " size must be " #Bytes " bytes (shader ABI)")

// ─── Enum helpers ───────────────────────────────────────────────────────────

template<EnumType E>
[[nodiscard]] constexpr auto to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

} // namespace ohao
