#pragma once
// Structured diagnostics and the state of one typed decode. Every error carries a
// stable code, a JSON Pointer path, the entity id (if any), the source position and,
// where it helps, what was expected, what was found and a "did you mean".
#include "reflect/json_value.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

class JsonWriter;
struct TypeInfo;

struct Diagnostic {
    std::string code;          // "E_UNKNOWN_FIELD", "W_UNKNOWN_COMPONENT", ...
    std::string path;          // JSON Pointer into the document ("" = the root)
    uint64_t entity{0};        // the entity's id, 0 when not inside an entity
    uint32_t offset{0};        // byte offset in the source
    uint32_t line{0};          // 1-based; 0 when there is no source text
    uint32_t column{0};        // 1-based, in bytes
    std::string message;
    std::string expected;
    std::string got;
    std::string suggestion;
};

struct ErrorCodeInfo {
    std::string_view code;
    std::string_view meaning;
};
// Every code a decode or load can report, with its meaning (part of the catalogue).
[[nodiscard]] std::span<const ErrorCodeInfo> errorCodes() noexcept;

// {"code", "path", "entity"?, "line"?, "column"?, "message", "expected"?, "got"?,
//  "suggestion"?}: optional members are written only when they carry something.
void writeDiagnostic(JsonWriter& w, const Diagnostic& d);

// Converts offsets to 1-based line/column against `source`.
void fillLineColumns(std::span<Diagnostic> diags, std::string_view source);

// The closest candidate within an edit distance of max(1, |word| / 3), or "".
[[nodiscard]] std::string suggest(std::string_view word, std::span<const std::string_view> candidates);

// "one of: a, b, c", cut after `max` names ("..., +N more") so errors stay terse.
[[nodiscard]] std::string oneOf(std::span<const std::string_view> names, std::size_t max = 12);

// An entity reference found while decoding; resolved once every entity is known.
struct PendingRef {
    void* field;                                // the decoded value to patch
    void (*assign)(void* field, uint64_t raw);  // writes a raw engine entity into it
    uint64_t id;                                // the referenced entity's id
    uint64_t entity;                            // id of the entity holding the reference
    const TypeInfo* component;                  // the component holding it
    // Its JSON Pointer is only needed for an error, so the context keeps the path
    // segments and DecodeContext::pathOf builds the string on demand. `path` is the
    // built form for a reference moved to another context (or given explicitly).
    std::string path;
    uint32_t offset;
    uint32_t pathBegin{0};
    uint32_t pathCount{0};
};

class DecodeContext {
public:
    explicit DecodeContext(const JsonDocument& doc, std::size_t maxErrors = 100);

    const JsonDocument& doc;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
    std::vector<PendingRef> refs;
    uint64_t entity{0};
    const TypeInfo* component{nullptr};
    // Validation without a world: decoded values are thrown away, so a number in a field
    // without limits is only checked to fit (from its decimal exponent), not converted.
    // The verdict is the same; readField turns conversion back on for fields with limits.
    bool valuesUnused{false};
    bool needValue{true};   // set per field by readField

    void push(std::string_view key) { m_path.push_back(Seg{key, 0, false}); }
    void push(uint32_t index) { m_path.push_back(Seg{{}, index, true}); }
    void pop() { m_path.pop_back(); }
    [[nodiscard]] std::string path() const;

    // Records an entity reference at the current path (resolved once every entity is known).
    void addRef(void* field, void (*assign)(void*, uint64_t), uint64_t id, uint32_t offset);
    // A reference's JSON Pointer. Valid while the document is: build it (into r.path)
    // before moving the reference out of this context.
    [[nodiscard]] std::string pathOf(const PendingRef& r) const;

    // After maxErrors errors one E_TOO_MANY_ERRORS is appended and further errors are
    // dropped; decoders check full() to stop early.
    void error(std::string_view code, const JsonNode& at, std::string message, std::string expected = {},
               std::string got = {}, std::string suggestion = {});
    void warning(std::string_view code, const JsonNode& at, std::string message, std::string suggestion = {});
    // A fully built diagnostic (path and position set by the caller), under the same cap.
    void report(Diagnostic d);
    [[nodiscard]] bool full() const noexcept { return m_full; }

    // A short rendering of a value for "got": the raw number, the quoted string, or the
    // compact JSON of a container, truncated.
    [[nodiscard]] std::string gotText(const JsonNode& n) const;

    struct Seg {
        std::string_view key;
        uint32_t index;
        bool isIndex;
    };
    // The store of saved ref paths, for decoders that run often and keep it warm: large
    // vectors freed after every decode go back to the OS and page-fault again on the
    // next one. adoptPaths takes a store's capacity; releasePaths hands it back, cleared.
    void adoptPaths(std::vector<Seg>&& store) {
        m_refPaths = std::move(store);
        m_refPaths.clear();
    }
    [[nodiscard]] std::vector<Seg> releasePaths() {
        m_refPaths.clear();
        return std::move(m_refPaths);
    }

private:
    Diagnostic make(std::string_view code, const JsonNode& at, std::string message) const;
    static std::string buildPath(const Seg* segs, std::size_t n);

    std::vector<Seg> m_path;
    std::vector<Seg> m_refPaths;   // the saved paths of refs, back to back
    std::size_t m_maxErrors;
    bool m_full{false};
};

// Scalar readers shared by the typed decoders. On failure they report and return false.
bool readFloat(const JsonNode& n, DecodeContext& ctx, float& out);
bool readInt(const JsonNode& n, DecodeContext& ctx, int64_t lo, int64_t hi, int64_t& out);
bool readHex64(const JsonNode& n, DecodeContext& ctx, std::string_view code, uint64_t& out);

} // namespace ohao::reflect
