#pragma once
// A 128-bit hash of content: what the cooked cache names its files by (docs/plans/2026-11-11-step5-assets.md).
// Two lanes of a multiply-and-fold over 8-byte words (the folded 64x64 -> 128 product wyhash and rapidhash use),
// each with its own constants, closed with the length. For naming a local cache, not for adversaries.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace ohao {

struct ContentHash {
    uint64_t hi{0}, lo{0};
    [[nodiscard]] friend bool operator==(const ContentHash&, const ContentHash&) = default;
    [[nodiscard]] friend auto operator<=>(const ContentHash&, const ContentHash&) = default;
    [[nodiscard]] explicit operator bool() const noexcept { return (hi | lo) != 0; }
    [[nodiscard]] std::string hex() const {
        static constexpr char digits[] = "0123456789abcdef";
        std::string s(32, '0');
        for (int i = 0; i < 16; ++i) {
            s[static_cast<std::size_t>(i)] = digits[(hi >> (60 - 4 * i)) & 15];
            s[static_cast<std::size_t>(16 + i)] = digits[(lo >> (60 - 4 * i)) & 15];
        }
        return s;
    }
};

class ContentHasher {
public:
    void bytes(const void* data, std::size_t size) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        m_length += size;
        if (m_pending) {   // fill the partial word first
            const std::size_t take = size < 8 - m_pending ? size : 8 - m_pending;
            std::memcpy(m_tail + m_pending, p, take);
            m_pending += take;
            p += take;
            size -= take;
            if (m_pending < 8) return;
            word(load(m_tail));
            m_pending = 0;
        }
        for (; size >= 8; p += 8, size -= 8) word(load(p));
        if (size) {
            std::memcpy(m_tail, p, size);
            m_pending = size;
        }
    }
    void bytes(std::span<const uint8_t> data) noexcept { bytes(data.data(), data.size()); }
    template <class T> void value(const T& v) noexcept { bytes(&v, sizeof v); }
    void text(std::string_view s) noexcept {
        value(static_cast<uint64_t>(s.size()));
        bytes(s.data(), s.size());
    }

    [[nodiscard]] ContentHash finish() const noexcept {
        uint64_t a = m_a, b = m_b;
        if (m_pending) {
            unsigned char last[8] = {};
            std::memcpy(last, m_tail, m_pending);
            const uint64_t w = load(last);
            a = fold(a ^ w, kA);
            b = fold(b ^ rotl(w, 29), kB);
        }
        ContentHash h;
        h.hi = fold(a ^ m_length, kB ^ b);
        h.lo = fold(b ^ rotl(m_length, 32) ^ kC, kA ^ a);
        return h;
    }

private:
    static constexpr uint64_t kA = 0x9E3779B97F4A7C15ull, kB = 0xD6E8FEB86659FD93ull, kC = 0xA0761D6478BD642Full;
    [[nodiscard]] static uint64_t fold(uint64_t x, uint64_t y) noexcept {
        const unsigned __int128 r = static_cast<unsigned __int128>(x) * y;
        return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
    }
    [[nodiscard]] static uint64_t rotl(uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    [[nodiscard]] static uint64_t load(const unsigned char* p) noexcept {
        uint64_t w;
        std::memcpy(&w, p, 8);
        return w;
    }
    void word(uint64_t w) noexcept {
        m_a = fold(m_a ^ w, kA) + kC;
        m_b = fold(m_b ^ rotl(w, 29), kB) + m_a;
    }

    uint64_t m_a{0x243F6A8885A308D3ull}, m_b{0x13198A2E03707344ull}, m_length{0};
    unsigned char m_tail[8]{};
    std::size_t m_pending{0};
};

[[nodiscard]] inline ContentHash contentHash(std::span<const uint8_t> data) noexcept {
    ContentHasher h;
    h.bytes(data);
    return h.finish();
}

} // namespace ohao
