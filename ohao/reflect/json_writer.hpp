#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

// Maps a raw engine entity value to its stable 64-bit EntityId (nullopt: dead / no id).
// Supplied by the ECS when writing scenes; reflect itself knows nothing about EnTT.
class EntityIdMapper {
public:
    virtual ~EntityIdMapper() = default;
    [[nodiscard]] virtual std::optional<uint64_t> idOf(uint64_t rawEntity) const = 0;
};

// Streaming writer of canonical compact JSON: no whitespace, numbers in their shortest
// round-trip form (std::to_chars), strings escaped one way only. The same calls always
// produce the same bytes. prettyPrint() turns its output into the file layout.
//
// A value that cannot be written canonically (NaN, an unregistered enum value) writes
// `null` and records the first error; check ok() after writing.
class JsonWriter {
public:
    explicit JsonWriter(std::size_t reserveBytes = 0) { m_out.reserve(reserveBytes); }

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(std::string_view k);

    void null();
    void boolean(bool v);
    void int64(int64_t v);
    void uint64(uint64_t v);
    void float32(float v);         // shortest float form; non-finite -> error
    void string(std::string_view s);
    void hex64(uint64_t v);        // "0123456789abcdef": 16 lowercase hex digits, quoted
    void raw(std::string_view compactJson);   // pre-canonical JSON, copied verbatim

    void fail(std::string message);
    [[nodiscard]] bool ok() const noexcept { return m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

    [[nodiscard]] const std::string& str() const noexcept { return m_out; }
    [[nodiscard]] std::string take() { return std::move(m_out); }

    const EntityIdMapper* entityIds{nullptr};

private:
    void separator();
    void appendString(std::string_view s);

    std::string m_out;
    std::string m_error;
    bool m_needComma{false};   // the previous token was a value or a closed container
};

// Canonical pretty layout of canonical compact JSON (as JsonWriter writes it):
// 2-space indent; a container prints on one line when it fits in `width` columns
// (including its key and trailing comma), else one member per line. The root container
// always breaks. One-line objects pad their braces ("{ "a": 1 }"), arrays do not.
// The result has no trailing newline. Removing whitespace outside strings gives back
// the input exactly.
[[nodiscard]] std::string prettyPrint(std::string_view compact, int width = 100);

// 64-bit FNV-1a.
[[nodiscard]] constexpr uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}

} // namespace ohao::reflect
