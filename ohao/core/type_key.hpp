#pragma once
// A C++ type's identity at run time, without RTTI: a 64-bit FNV-1a hash of its name as the compiler spells it
// (__PRETTY_FUNCTION__; MSVC's __FUNCSIG__), so the same type has the same key in the executable, in a game module, and in the next build
// of that module loaded beside it (step 3j's reload), as EnTT's type_hash keys its pools. The reflection registry
// finds a type's TypeInfo by it, and the World's event log names a channel's payload type by it, so ECS systems hold a
// type's identity without including reflect/.
#include <cstdint>
#include <string_view>

namespace ohao {

using TypeKey = uint64_t;

#if defined(_MSC_VER) && !defined(__clang__)
#define OHAO_TYPE_SIGNATURE __FUNCSIG__
#else
#define OHAO_TYPE_SIGNATURE __PRETTY_FUNCTION__
#endif

namespace detail {
constexpr uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (const char c : s) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return h;
}
} // namespace detail

template <class T>
constexpr TypeKey typeKey() noexcept {
    constexpr TypeKey key = detail::fnv1a64(OHAO_TYPE_SIGNATURE);   // "... [with T = <the type>]"; at compile time
    return key;
}

// The type's name as the compiler spells it ("ohao::ecs::Health"), for messages.
template <class T>
constexpr std::string_view typeName() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    constexpr std::string_view f = __FUNCSIG__;   // "... __cdecl ohao::typeName<struct <the type>>(void) noexcept"
    constexpr std::size_t open = f.find("typeName<") + 9;
    constexpr std::size_t end = f.rfind(">(void)");
    constexpr std::string_view t = f.substr(open, end - open);
    for (const std::string_view tag : {"struct ", "class ", "enum ", "union "})
        if (t.starts_with(tag)) return t.substr(tag.size());
    return t;
#else
    constexpr std::string_view f = __PRETTY_FUNCTION__;   // "... typeName() [with T = <the type>; ...]"
    constexpr std::size_t at = f.find("T = ") + 4;
    constexpr std::size_t end = f.find_first_of(";]", at);
    return f.substr(at, end - at);
#endif
}

} // namespace ohao
