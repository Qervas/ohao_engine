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

// The two canonical renderings of one document. Both are deterministic: the same calls
// always produce the same bytes.
enum class JsonLayout : uint8_t {
    // No whitespace at all. For the wire, queries and hashing.
    Compact,
    // The file layout: 2-space indent; a container prints on one line when it fits in
    // `width` columns (counting its key and trailing comma), else one member per line.
    // The root container always breaks. One-line objects pad their braces
    // ("{ "a": 1 }"), arrays do not. Identical to prettyPrint() of the compact form.
    Pretty,
};

// Streaming writer of canonical JSON. Numbers are in their shortest round-trip form
// (std::to_chars), strings are escaped one way only.
//
// Pretty layout is produced in the same single pass as the content: each container is
// written in its one-line form, and when it closes and turns out not to fit (or contains
// a broken child), it is re-laid out in place, at the tail of the buffer. Nothing is
// scanned twice; a broken container's bytes are copied once per broken ancestor.
//
// A value that cannot be written canonically (NaN, an unregistered enum value) writes
// `null` and records the first error; check ok() after writing.
class JsonWriter {
public:
    explicit JsonWriter(std::size_t reserveBytes = 0, JsonLayout layout = JsonLayout::Compact, int width = 100);

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
    void numberToken(std::string_view token);  // an already-valid JSON number, verbatim
    // Canonical compact JSON (as this writer writes it in Compact layout), inserted as
    // one value and laid out like every other value.
    void raw(std::string_view compactJson);

    void fail(std::string message);
    [[nodiscard]] bool ok() const noexcept { return m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

    [[nodiscard]] const std::string& str() const noexcept { return m_out; }
    [[nodiscard]] std::string take() { return std::move(m_out); }
    [[nodiscard]] JsonLayout layout() const noexcept { return m_layout; }

    const EntityIdMapper* entityIds{nullptr};

private:
    struct Frame {
        uint32_t start;      // offset of '{' / '['
        uint32_t column;     // column of the bracket when the parent is broken
        uint32_t sepBegin;   // this frame's separators in m_seps start here
        uint32_t end;        // offset of the closing bracket (pending frames)
        uint16_t depth;
        bool object;
        bool hasChildren;
        bool brokenChild;    // a direct child was laid out broken: this one must break
    };

    void beforeValue();
    void appendString(std::string_view s);
    void openContainer(bool object);
    void closeContainer(bool object);
    void resolvePending(bool trailingComma);
    void breakContainer(const Frame& f);
    std::size_t rawValue(std::string_view s, std::size_t i);

    std::string m_out;
    std::string m_error;
    JsonLayout m_layout;
    uint32_t m_width;
    bool m_needComma{false};   // compact: the previous token was a value or a closed container
    // pretty layout state
    bool m_afterKey{false};
    uint32_t m_nextColumn{0};
    std::vector<Frame> m_frames;   // open containers
    std::vector<uint32_t> m_seps;  // offsets of ", " between direct children, per frame
    Frame m_pending{};             // last closed container, waiting for its fit decision
    bool m_hasPending{false};
    std::string m_scratch;
};

// Canonical pretty layout of canonical compact JSON (see JsonLayout::Pretty). The result
// has no trailing newline. Removing whitespace outside strings gives back the input.
[[nodiscard]] std::string prettyPrint(std::string_view compact, int width = 100);

// 64-bit FNV-1a.
[[nodiscard]] constexpr uint64_t fnv1a64(std::string_view s) noexcept {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}

} // namespace ohao::reflect
