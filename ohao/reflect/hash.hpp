#pragma once
// A 64-bit hash of reflected values (world.hash, sub-project 4 spec "Determinism"). Values
// are fed field by field through their registered types, never as raw object bytes, so
// padding and heap pointers cannot reach the hash, floats count by their bits (as equality
// does: -0 != +0), and entity references count by the target's EntityId, not the handle.
//
// The function is FNV-1a 64 taken over 8-byte words instead of bytes, with a xor-shift fold
// after each multiply. Each word's step, h -> ((h ^ w) * p) ^ (((h ^ w) * p) >> 32), is a
// bijection of h, so two inputs that differ in a single word always hash differently; a
// word per step makes it eight times faster than byte-wise FNV over the same data. A final
// splitmix64 spreads every bit over the result.
#include "reflect/json_writer.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace ohao::reflect {

class Hasher {
public:
    // Entity references hash as the target's id (0 when it has none). Without a mapper
    // they hash as 0.
    const EntityIdMapper* entityIds{nullptr};

    void u32(uint32_t v) noexcept {
        if (m_half) {
            mix(m_pending | (static_cast<uint64_t>(v) << 32));
            m_half = false;
        } else {
            m_pending = v;
            m_half = true;
        }
        ++m_units;
    }
    void u64(uint64_t v) noexcept {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    // `n` bytes, n a multiple of 4: the same as u32() per 4 bytes (little-endian), eight
    // bytes per step. For values whose bytes are exactly their fields (no padding).
    void words(const void* data, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(data);
        std::size_t i = 0;
        if (m_half && n >= 4) {
            uint32_t w;
            std::memcpy(&w, b, 4);
            u32(w);
            i = 4;
        }
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, b + i, 8);
            mix(w);
            m_units += 2;
        }
        if (i < n) {
            uint32_t w;
            std::memcpy(&w, b + i, 4);
            u32(w);
        }
    }
    void f32(float v) noexcept {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    // Length first, so "ab" + "c" and "a" + "bc" differ; the bytes in 4-byte units, the
    // last one zero-padded.
    void string(std::string_view s) noexcept {
        u32(static_cast<uint32_t>(s.size()));
        std::size_t i = 0;
        for (; i + 4 <= s.size(); i += 4) {
            uint32_t w;
            std::memcpy(&w, s.data() + i, 4);
            u32(w);
        }
        if (i < s.size()) {
            uint32_t w = 0;
            std::memcpy(&w, s.data() + i, s.size() - i);
            u32(w);
        }
    }
    void entity(uint64_t rawEntity) noexcept {
        const auto id = entityIds ? entityIds->idOf(rawEntity) : std::nullopt;
        u64(id ? *id : 0);
    }

    [[nodiscard]] uint64_t finish() const noexcept {
        uint64_t h = m_h;
        if (m_half) h = step(h, m_pending);
        h = step(h, m_units);   // the length of the whole input
        h += 0x9e3779b97f4a7c15ull;
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        return h ^ (h >> 31);
    }

private:
    static constexpr uint64_t step(uint64_t h, uint64_t w) noexcept {
        h = (h ^ w) * 0x100000001b3ull;
        return h ^ (h >> 32);
    }
    void mix(uint64_t w) noexcept { m_h = step(m_h, w); }

    uint64_t m_h{0xcbf29ce484222325ull};   // the FNV-1a 64 offset basis
    uint64_t m_pending{0};
    uint64_t m_units{0};
    bool m_half{false};
};

} // namespace ohao::reflect
