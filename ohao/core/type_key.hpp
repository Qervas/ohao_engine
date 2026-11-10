#pragma once
// A C++ type's identity at run time, without RTTI: a 64-bit FNV-1a hash of its name as the compiler spells it
// (__PRETTY_FUNCTION__), so the same type has the same key in the executable, in a game module, and in the next build
// of that module loaded beside it (step 3j's reload), as EnTT's type_hash keys its pools. The reflection registry
// finds a type's TypeInfo by it, and the World's event log names a channel's payload type by it, so ECS systems hold a
// type's identity without including reflect/.
#include <cstdint>
#include <string_view>

namespace ohao {

using TypeKey = uint64_t;

namespace detail {
constexpr uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (const char c : s) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return h;
}
} // namespace detail

template <class T>
constexpr TypeKey typeKey() noexcept {
    constexpr TypeKey key = detail::fnv1a64(__PRETTY_FUNCTION__);   // "... [with T = <the type>]"; at compile time
    return key;
}

} // namespace ohao
