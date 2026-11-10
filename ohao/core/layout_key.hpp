#pragma once
// A type's memory layout as a 64-bit key, at compile time (step 3j): what two builds of a game must agree on before
// data written by one is read by the other in place (a reload: game/reload.hpp). An aggregate's key is its size, its
// alignment and its members' keys in order (structured bindings over a field count found by brace initialization,
// the boost.pfr technique), so adding, removing, retyping or reordering a member changes it. std::vector,
// std::optional, std::shared_ptr and arrays key by their element's; anything else (scalars, enums, glm, strings,
// classes with private members, aggregates past kMaxFields) by its name and size.
#include "core/type_key.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace ohao {

template <class T>
constexpr uint64_t layoutKey() noexcept;

namespace detail {

constexpr uint64_t mixKey(uint64_t h, uint64_t v) noexcept {
    return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 1099511628211ull;
}

// Converts to any member type; only named in unevaluated operands.
struct AnyMember {
    template <class U>
    constexpr operator U() const noexcept;
};

template <class T, std::size_t... I>
constexpr bool bracesTake(std::index_sequence<I...>) noexcept {
    return requires { T{(void(I), AnyMember{})...}; };
}

inline constexpr std::size_t kMaxFields = 24;

// The aggregate's member count: the most initializers T{...} takes (members of aggregate type take one each, by
// AnyMember's conversion, so there is no brace elision).
template <class T, std::size_t N = 0>
constexpr std::size_t fieldCount() noexcept {
    if constexpr (N > kMaxFields) return N;
    else if constexpr (bracesTake<T>(std::make_index_sequence<N + 1>{})) return fieldCount<T, N + 1>();
    else return N;
}

template <class... M>
struct Members {
    static constexpr uint64_t key(uint64_t h) noexcept {
        ((h = mixKey(h, layoutKey<M>())), ...);
        return h;
    }
};

// The member types of an aggregate with N members (a lambda's deduced return type, never called).
#define OHAO_LAYOUT_CASE(N, ...)                                                                          \
    else if constexpr (n == N) return decltype([](T& t) {                                                  \
        auto& [__VA_ARGS__] = t;                                                                           \
        return Members<OHAO_LAYOUT_DECLTYPES(__VA_ARGS__)>{};                                              \
    }(std::declval<T&>())){};
#define OHAO_LAYOUT_D1(a) decltype(a)
#define OHAO_LAYOUT_EXPAND(x) x
#define OHAO_LAYOUT_DECLTYPES(...) OHAO_LAYOUT_EXPAND(OHAO_LAYOUT_DT_PICK(__VA_ARGS__, OHAO_LAYOUT_DT24, OHAO_LAYOUT_DT23, \
    OHAO_LAYOUT_DT22, OHAO_LAYOUT_DT21, OHAO_LAYOUT_DT20, OHAO_LAYOUT_DT19, OHAO_LAYOUT_DT18, OHAO_LAYOUT_DT17,        \
    OHAO_LAYOUT_DT16, OHAO_LAYOUT_DT15, OHAO_LAYOUT_DT14, OHAO_LAYOUT_DT13, OHAO_LAYOUT_DT12, OHAO_LAYOUT_DT11,        \
    OHAO_LAYOUT_DT10, OHAO_LAYOUT_DT9, OHAO_LAYOUT_DT8, OHAO_LAYOUT_DT7, OHAO_LAYOUT_DT6, OHAO_LAYOUT_DT5,             \
    OHAO_LAYOUT_DT4, OHAO_LAYOUT_DT3, OHAO_LAYOUT_DT2, OHAO_LAYOUT_DT1)(__VA_ARGS__))
#define OHAO_LAYOUT_DT_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20,  \
                            _21, _22, _23, _24, NAME, ...) NAME
#define OHAO_LAYOUT_DT1(a) decltype(a)
#define OHAO_LAYOUT_DT2(a, ...) decltype(a), OHAO_LAYOUT_DT1(__VA_ARGS__)
#define OHAO_LAYOUT_DT3(a, ...) decltype(a), OHAO_LAYOUT_DT2(__VA_ARGS__)
#define OHAO_LAYOUT_DT4(a, ...) decltype(a), OHAO_LAYOUT_DT3(__VA_ARGS__)
#define OHAO_LAYOUT_DT5(a, ...) decltype(a), OHAO_LAYOUT_DT4(__VA_ARGS__)
#define OHAO_LAYOUT_DT6(a, ...) decltype(a), OHAO_LAYOUT_DT5(__VA_ARGS__)
#define OHAO_LAYOUT_DT7(a, ...) decltype(a), OHAO_LAYOUT_DT6(__VA_ARGS__)
#define OHAO_LAYOUT_DT8(a, ...) decltype(a), OHAO_LAYOUT_DT7(__VA_ARGS__)
#define OHAO_LAYOUT_DT9(a, ...) decltype(a), OHAO_LAYOUT_DT8(__VA_ARGS__)
#define OHAO_LAYOUT_DT10(a, ...) decltype(a), OHAO_LAYOUT_DT9(__VA_ARGS__)
#define OHAO_LAYOUT_DT11(a, ...) decltype(a), OHAO_LAYOUT_DT10(__VA_ARGS__)
#define OHAO_LAYOUT_DT12(a, ...) decltype(a), OHAO_LAYOUT_DT11(__VA_ARGS__)
#define OHAO_LAYOUT_DT13(a, ...) decltype(a), OHAO_LAYOUT_DT12(__VA_ARGS__)
#define OHAO_LAYOUT_DT14(a, ...) decltype(a), OHAO_LAYOUT_DT13(__VA_ARGS__)
#define OHAO_LAYOUT_DT15(a, ...) decltype(a), OHAO_LAYOUT_DT14(__VA_ARGS__)
#define OHAO_LAYOUT_DT16(a, ...) decltype(a), OHAO_LAYOUT_DT15(__VA_ARGS__)
#define OHAO_LAYOUT_DT17(a, ...) decltype(a), OHAO_LAYOUT_DT16(__VA_ARGS__)
#define OHAO_LAYOUT_DT18(a, ...) decltype(a), OHAO_LAYOUT_DT17(__VA_ARGS__)
#define OHAO_LAYOUT_DT19(a, ...) decltype(a), OHAO_LAYOUT_DT18(__VA_ARGS__)
#define OHAO_LAYOUT_DT20(a, ...) decltype(a), OHAO_LAYOUT_DT19(__VA_ARGS__)
#define OHAO_LAYOUT_DT21(a, ...) decltype(a), OHAO_LAYOUT_DT20(__VA_ARGS__)
#define OHAO_LAYOUT_DT22(a, ...) decltype(a), OHAO_LAYOUT_DT21(__VA_ARGS__)
#define OHAO_LAYOUT_DT23(a, ...) decltype(a), OHAO_LAYOUT_DT22(__VA_ARGS__)
#define OHAO_LAYOUT_DT24(a, ...) decltype(a), OHAO_LAYOUT_DT23(__VA_ARGS__)

template <class T>
constexpr auto members() noexcept {
    constexpr std::size_t n = fieldCount<T>();
    if constexpr (n == 0) return Members<>{};
    OHAO_LAYOUT_CASE(1, m1)
    OHAO_LAYOUT_CASE(2, m1, m2)
    OHAO_LAYOUT_CASE(3, m1, m2, m3)
    OHAO_LAYOUT_CASE(4, m1, m2, m3, m4)
    OHAO_LAYOUT_CASE(5, m1, m2, m3, m4, m5)
    OHAO_LAYOUT_CASE(6, m1, m2, m3, m4, m5, m6)
    OHAO_LAYOUT_CASE(7, m1, m2, m3, m4, m5, m6, m7)
    OHAO_LAYOUT_CASE(8, m1, m2, m3, m4, m5, m6, m7, m8)
    OHAO_LAYOUT_CASE(9, m1, m2, m3, m4, m5, m6, m7, m8, m9)
    OHAO_LAYOUT_CASE(10, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10)
    OHAO_LAYOUT_CASE(11, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11)
    OHAO_LAYOUT_CASE(12, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12)
    OHAO_LAYOUT_CASE(13, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13)
    OHAO_LAYOUT_CASE(14, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14)
    OHAO_LAYOUT_CASE(15, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15)
    OHAO_LAYOUT_CASE(16, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16)
    OHAO_LAYOUT_CASE(17, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17)
    OHAO_LAYOUT_CASE(18, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18)
    OHAO_LAYOUT_CASE(19, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19)
    OHAO_LAYOUT_CASE(20, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20)
    OHAO_LAYOUT_CASE(21, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21)
    OHAO_LAYOUT_CASE(22, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21,
                     m22)
    OHAO_LAYOUT_CASE(23, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21,
                     m22, m23)
    OHAO_LAYOUT_CASE(24, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21,
                     m22, m23, m24)
    else return Members<>{};
}

template <class T>
struct IsVector : std::false_type {};
template <class E, class A>
struct IsVector<std::vector<E, A>> : std::true_type {};
template <class T>
struct IsOptional : std::false_type {};
template <class E>
struct IsOptional<std::optional<E>> : std::true_type {};
template <class T>
struct IsSharedPtr : std::false_type {};
template <class E>
struct IsSharedPtr<std::shared_ptr<E>> : std::true_type {};

} // namespace detail

template <class T>
constexpr uint64_t layoutKey() noexcept {
    using namespace detail;
    using U = std::remove_cv_t<T>;
    constexpr uint64_t base = mixKey(mixKey(1469598103934665603ull, sizeof(U)), alignof(U));
    if constexpr (std::is_array_v<U>)
        return mixKey(mixKey(base, std::extent_v<U>), layoutKey<std::remove_extent_t<U>>());
    else if constexpr (IsVector<U>::value || IsOptional<U>::value)
        return mixKey(mixKey(base, typeKey<U>()), layoutKey<typename U::value_type>());
    else if constexpr (IsSharedPtr<U>::value)
        return mixKey(base, typeKey<U>());   // the pointee is its own object, not this one's layout
    else if constexpr (std::is_aggregate_v<U> && std::is_class_v<U>) {
        constexpr std::size_t n = fieldCount<U>();
        if constexpr (n > kMaxFields) return mixKey(base, typeKey<U>());
        else return decltype(members<U>())::key(mixKey(base, n));
    } else
        return mixKey(base, typeKey<U>());
}

} // namespace ohao

#undef OHAO_LAYOUT_CASE
#undef OHAO_LAYOUT_D1
