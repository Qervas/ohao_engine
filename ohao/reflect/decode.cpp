#include "reflect/decode.hpp"

#include "reflect/json_writer.hpp"
#include "reflect/type_info.hpp"

#include <fast_float/fast_float.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace ohao::reflect {

// ---- JSON tree helpers ------------------------------------------------------------------

const char* jsonTypeName(JsonType t) noexcept {
    switch (t) {
    case JsonType::Null: return "null";
    case JsonType::Bool: return "boolean";
    case JsonType::Number: return "number";
    case JsonType::String: return "string";
    case JsonType::Array: return "array";
    case JsonType::Object: return "object";
    }
    return "?";
}

const JsonNode* JsonDocument::member(const JsonNode& object, std::string_view k) const {
    if (object.type != JsonType::Object) return nullptr;
    for (uint32_t i = 0; i < object.count; ++i) {
        const JsonNode& m = child(object, i);
        if (m.key == k) return &m;
    }
    return nullptr;
}

void writeJson(JsonWriter& w, const JsonDocument& doc, const JsonNode& n) {
    switch (n.type) {
    case JsonType::Null: w.null(); break;
    case JsonType::Bool: w.boolean(n.boolean); break;
    case JsonType::Number: w.numberToken(n.text); break;
    case JsonType::String: w.string(n.text); break;
    case JsonType::Array:
        w.beginArray();
        for (uint32_t i = 0; i < n.count; ++i) writeJson(w, doc, doc.child(n, i));
        w.endArray();
        break;
    case JsonType::Object:
        w.beginObject();
        for (uint32_t i = 0; i < n.count; ++i) {
            const JsonNode& m = doc.child(n, i);
            w.key(m.key);
            writeJson(w, doc, m);
        }
        w.endObject();
        break;
    }
}

bool isJsonNumber(std::string_view s, bool* isInteger) noexcept {
    std::size_t i = 0;
    const std::size_t n = s.size();
    const auto digit = [&](std::size_t k) { return k < n && s[k] >= '0' && s[k] <= '9'; };
    if (i < n && s[i] == '-') ++i;
    if (!digit(i)) return false;
    if (s[i] == '0') ++i;
    else while (digit(i)) ++i;
    bool integer = true;
    if (i < n && s[i] == '.') {
        integer = false;
        ++i;
        if (!digit(i)) return false;
        while (digit(i)) ++i;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        integer = false;
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        if (!digit(i)) return false;
        while (digit(i)) ++i;
    }
    if (isInteger) *isInteger = integer;
    return i == n;
}

// ---- codes, rendering, positions ---------------------------------------------------------

namespace {
constexpr std::array<ErrorCodeInfo, 25> kCodes = {{
    {"E_IO", "The file could not be read or written."},
    {"E_PARSE", "The text is not valid JSON (or not valid UTF-8)."},
    {"E_FORMAT", "Not an OHAO scene: \"format\" is missing or not \"ohao.scene\", or \"version\" or \"entities\" is missing."},
    {"E_VERSION_UNSUPPORTED", "The document version is not one this engine reads."},
    {"E_COMPONENT_VERSION", "A component's version in the file is newer than the engine's, or older with no upgrade path."},
    {"E_UPGRADE", "An upgrade (migration) rejected a component written by an older version."},
    {"E_UNKNOWN_FIELD", "A member the type does not have, or a runtime-only component in a file."},
    {"E_DUPLICATE_KEY", "The same member appears twice in one object."},
    {"E_TYPE", "A value of the wrong JSON type or shape."},
    {"E_ENUM", "A string that is not a value of the enum."},
    {"E_RANGE", "A number outside the field's limits or its type's range."},
    {"E_NONFINITE", "NaN or infinity, which a scene cannot contain."},
    {"E_ID_INVALID", "A missing, malformed or zero entity id (ids are 16 lowercase hex digits)."},
    {"E_ID_DUPLICATE", "Two entities with the same id."},
    {"E_PARENT_MISSING", "A Parent that names no entity in the document."},
    {"E_PARENT_CYCLE", "Parents that form a cycle."},
    {"E_SINGLETON", "A second instance of a component that allows one per world."},
    {"E_ENTITY_NOT_FOUND", "An entity reference that names no entity in the document."},
    {"E_WORLD_NOT_EMPTY", "Loading into a world that already has entities."},
    {"E_TOO_MANY_ERRORS", "More errors exist than are reported (the first 100 are)."},
    {"E_METHOD", "An unknown API method."},
    {"E_COMMAND", "A request that is malformed or cannot apply (unknown op, missing member, absent component)."},
    {"E_COMPONENT_UNKNOWN", "A request names a type that is not a component (or, for edits, not a saved one)."},
    {"E_PATH", "A path that reaches no field: \"/Type/field\", then optionally member names or array indices."},
    {"W_UNKNOWN_COMPONENT", "Warning: an unknown component close to a known name. It is kept, but may be a typo."},
}};
} // namespace

std::span<const ErrorCodeInfo> errorCodes() noexcept { return kCodes; }

void writeDiagnostic(JsonWriter& w, const Diagnostic& d) {
    w.beginObject();
    w.key("code"); w.string(d.code);
    w.key("path"); w.string(d.path);
    if (d.entity) { w.key("entity"); w.hex64(d.entity); }
    if (d.line) {
        w.key("line"); w.int64(d.line);
        w.key("column"); w.int64(d.column);
    }
    w.key("message"); w.string(d.message);
    if (!d.expected.empty()) { w.key("expected"); w.string(d.expected); }
    if (!d.got.empty()) { w.key("got"); w.string(d.got); }
    if (!d.suggestion.empty()) { w.key("suggestion"); w.string(d.suggestion); }
    w.endObject();
}

void fillLineColumns(std::span<Diagnostic> diags, std::string_view source) {
    if (diags.empty() || source.empty()) return;
    std::vector<uint32_t> lineStarts{0};
    for (uint32_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n') lineStarts.push_back(i + 1);
    for (auto& d : diags) {
        const uint32_t off = std::min<uint32_t>(d.offset, static_cast<uint32_t>(source.size()));
        const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), off);
        const auto line = static_cast<uint32_t>(it - lineStarts.begin());
        d.line = line;
        d.column = off - lineStarts[line - 1] + 1;
    }
}

std::string suggest(std::string_view word, std::span<const std::string_view> candidates) {
    const std::size_t limit = std::max<std::size_t>(1, word.size() / 3);
    std::size_t best = limit + 1;
    std::string_view pick;
    const auto lower = [](char ch) { return (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch; };
    std::vector<std::size_t> d;
    for (std::string_view c : candidates) {
        // Optimal string alignment distance, case-insensitive: insertions, deletions,
        // substitutions and swaps of adjacent letters ("Spto" -> "Spot") each cost 1.
        const std::size_t w = c.size() + 1;
        d.assign((word.size() + 1) * w, 0);
        for (std::size_t j = 0; j <= c.size(); ++j) d[j] = j;
        for (std::size_t i = 1; i <= word.size(); ++i) {
            d[i * w] = i;
            for (std::size_t j = 1; j <= c.size(); ++j) {
                const bool same = lower(word[i - 1]) == lower(c[j - 1]);
                std::size_t v = std::min({d[(i - 1) * w + j] + 1, d[i * w + j - 1] + 1, d[(i - 1) * w + j - 1] + (same ? 0 : 1)});
                if (i > 1 && j > 1 && lower(word[i - 1]) == lower(c[j - 2]) && lower(word[i - 2]) == lower(c[j - 1]))
                    v = std::min(v, d[(i - 2) * w + j - 2] + 1);
                d[i * w + j] = v;
            }
        }
        const std::size_t dist = d[word.size() * w + c.size()];
        if (dist < best) {
            best = dist;
            pick = c;
        }
    }
    return best <= limit ? std::string(pick) : std::string();
}

std::string oneOf(std::span<const std::string_view> names, std::size_t max) {
    std::string s = "one of: ";
    for (std::size_t i = 0; i < names.size() && i < max; ++i) {
        if (i) s += ", ";
        s += names[i];
    }
    if (names.size() > max) s += ", +" + std::to_string(names.size() - max) + " more";
    return s;
}

// ---- DecodeContext -----------------------------------------------------------------------

DecodeContext::DecodeContext(const JsonDocument& d, std::size_t maxErrors) : doc(d), m_maxErrors(maxErrors) {}

std::string DecodeContext::path() const {
    std::string p;
    for (const Seg& s : m_path) {
        p.push_back('/');
        if (s.isIndex) {
            p += std::to_string(s.index);
            continue;
        }
        for (char c : s.key) {   // JSON Pointer escaping (RFC 6901)
            if (c == '~') p += "~0";
            else if (c == '/') p += "~1";
            else p.push_back(c);
        }
    }
    return p;
}

Diagnostic DecodeContext::make(std::string_view code, const JsonNode& at, std::string message) const {
    Diagnostic d;
    d.code = code;
    d.path = path();
    d.entity = entity;
    d.offset = at.offset;
    d.message = std::move(message);
    return d;
}

void DecodeContext::error(std::string_view code, const JsonNode& at, std::string message, std::string expected,
                          std::string got, std::string suggestion) {
    if (m_full) return;
    if (errors.size() >= m_maxErrors) {
        m_full = true;
        Diagnostic d = make("E_TOO_MANY_ERRORS", at, "more errors exist; the first " + std::to_string(m_maxErrors) +
                                                         " are reported");
        errors.push_back(std::move(d));
        return;
    }
    Diagnostic d = make(code, at, std::move(message));
    d.expected = std::move(expected);
    d.got = std::move(got);
    d.suggestion = std::move(suggestion);
    errors.push_back(std::move(d));
}

void DecodeContext::report(Diagnostic d) {
    if (m_full) return;
    if (errors.size() >= m_maxErrors) {
        m_full = true;
        Diagnostic t;
        t.code = "E_TOO_MANY_ERRORS";
        t.offset = d.offset;
        t.message = "more errors exist; the first " + std::to_string(m_maxErrors) + " are reported";
        errors.push_back(std::move(t));
        return;
    }
    errors.push_back(std::move(d));
}

void DecodeContext::warning(std::string_view code, const JsonNode& at, std::string message, std::string sugg) {
    Diagnostic d = make(code, at, std::move(message));
    d.suggestion = std::move(sugg);
    warnings.push_back(std::move(d));
}

std::string DecodeContext::gotText(const JsonNode& n) const {
    std::string s;
    switch (n.type) {
    case JsonType::Number: s = n.text; break;
    case JsonType::Null: s = "null"; break;
    case JsonType::Bool: s = n.boolean ? "true" : "false"; break;
    default: {
        JsonWriter w;
        writeJson(w, doc, n);
        s = w.take();
    }
    }
    constexpr std::size_t kMax = 80;
    if (s.size() > kMax) {
        s.resize(kMax);
        s += "...";
    }
    return s;
}

// ---- scalar readers -----------------------------------------------------------------------

bool readFloat(const JsonNode& n, DecodeContext& ctx, float& out) {
    if (n.type != JsonType::Number) {
        ctx.error("E_TYPE", n, std::string("expected a number, found ") + jsonTypeName(n.type), "number",
                  ctx.gotText(n));
        return false;
    }
    // Correctly rounded decimal -> float, straight from the source token (never via a
    // double). Same results as std::from_chars<float>, several times faster.
    const char* first = n.text.data();
    const char* last = first + n.text.size();
    const auto r = fast_float::from_chars(first, last, out);
    if (r.ec != std::errc{} && r.ec != std::errc::result_out_of_range) {
        ctx.error("E_TYPE", n, "not a number", "number", std::string(n.text));
        return false;
    }
    // Overflow gives infinity; underflow of a nonzero literal gives 0. Both mean the
    // number does not fit.
    bool underflow = false;
    if (out == 0.0f)
        for (const char* p = first; p < last && *p != 'e' && *p != 'E'; ++p) underflow |= (*p >= '1' && *p <= '9');
    if (r.ec == std::errc::result_out_of_range || !std::isfinite(out) || underflow || r.ptr != last) {
        ctx.error("E_RANGE", n, "the number does not fit in a 32-bit float", "a finite 32-bit float",
                  std::string(n.text));
        return false;
    }
    return true;
}

bool readInt(const JsonNode& n, DecodeContext& ctx, int64_t lo, int64_t hi, int64_t& out) {
    bool integer = false;
    if (n.type != JsonType::Number || !isJsonNumber(n.text, &integer) || !integer) {
        ctx.error("E_TYPE", n, "expected an integer", "integer", ctx.gotText(n));
        return false;
    }
    const auto r = std::from_chars(n.text.data(), n.text.data() + n.text.size(), out);
    if (r.ec != std::errc{} || out < lo || out > hi) {
        ctx.error("E_RANGE", n, "the integer is out of range", "an integer in [" + std::to_string(lo) + ", " +
                                                                    std::to_string(hi) + "]",
                  std::string(n.text));
        return false;
    }
    return true;
}

bool readHex64(const JsonNode& n, DecodeContext& ctx, std::string_view code, uint64_t& out) {
    bool ok = n.type == JsonType::String && n.text.size() == 16;
    uint64_t v = 0;
    for (std::size_t i = 0; ok && i < 16; ++i) {
        const char c = n.text[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else { ok = false; break; }
        v = (v << 4) | d;
    }
    if (!ok) {
        ctx.error(code, n, "expected 16 lowercase hex digits", "\"0123456789abcdef\"", ctx.gotText(n));
        return false;
    }
    out = v;
    return true;
}

// ---- struct decoding ----------------------------------------------------------------------

namespace {

std::string limitsText(const FieldInfo& f) {
    const auto num = [&](double v) {
        JsonWriter w;
        const Kind k = (f.type->kind == Kind::Vector || f.type->kind == Kind::Optional) && f.type->element
                           ? f.type->element->kind
                           : f.type->kind;
        if (k == Kind::Int32 || k == Kind::UInt32) w.int64(static_cast<int64_t>(v));
        else w.float32(static_cast<float>(v));
        return w.take();
    };
    const auto& l = f.attrs.limits;
    std::string s = isNumeric(f.type->kind) && f.type->kind != Kind::Int32 && f.type->kind != Kind::UInt32 &&
                            f.type->kind != Kind::Float
                        ? "each component "
                        : "";
    if (l.min && l.max) return s + "in [" + num(*l.min) + ", " + num(*l.max) + "]";
    if (l.min) return s + ">= " + num(*l.min);
    return s + "<= " + num(*l.max);
}

int fieldIndex(const TypeInfo& t, std::string_view key, uint32_t position) {
    // Canonical files list fields in registration order: try the member's own position
    // first, so a canonical file never searches.
    if (position < t.fields.size() && t.fields[position].name == key) return static_cast<int>(position);
    for (std::size_t i = 0; i < t.fields.size(); ++i) {
        const FieldInfo& f = t.fields[i];
        if (f.name == key) return static_cast<int>(i);
        for (const auto& a : f.attrs.aliases)
            if (a == key) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

void readStruct(void* obj, const TypeInfo& t, const JsonNode& n, DecodeContext& ctx) {
    if (n.type != JsonType::Object) {
        ctx.error("E_TYPE", n, t.name + " must be an object", "object", ctx.gotText(n));
        return;
    }
    uint64_t seenSmall = 0;
    std::vector<bool> seenLarge;
    if (t.fields.size() > 64) seenLarge.assign(t.fields.size(), false);
    for (uint32_t i = 0; i < n.count && !ctx.full(); ++i) {
        const JsonNode& m = ctx.doc.child(n, i);
        ctx.push(m.key);
        const int idx = fieldIndex(t, m.key, i);
        if (idx < 0) {
            std::vector<std::string_view> names;
            for (const auto& f : t.fields) names.push_back(f.name);
            ctx.error("E_UNKNOWN_FIELD", m, t.name + " has no field '" + std::string(m.key) + "'", oneOf(names),
                      "\"" + std::string(m.key) + "\"", suggest(m.key, names));
            ctx.pop();
            continue;
        }
        bool dup;
        if (seenLarge.empty()) {
            dup = (seenSmall >> idx) & 1u;
            seenSmall |= uint64_t{1} << idx;
        } else {
            dup = seenLarge[static_cast<std::size_t>(idx)];
            seenLarge[static_cast<std::size_t>(idx)] = true;
        }
        const FieldInfo& f = t.fields[static_cast<std::size_t>(idx)];
        if (dup) {
            ctx.error("E_DUPLICATE_KEY", m, "field '" + f.name + "' is given twice (or by name and alias)");
            ctx.pop();
            continue;
        }
        readField(obj, t, f, m, ctx);
        ctx.pop();
    }
}

bool readField(void* obj, const TypeInfo& t, const FieldInfo& f, const JsonNode& n, DecodeContext& ctx) {
    const std::size_t before = ctx.errors.size();
    f.ops->read(obj, n, ctx, *f.type);
    const auto& l = f.attrs.limits;
    if (ctx.errors.size() == before && (l.min || l.max) && !f.ops->inRange(obj, l))
        ctx.error("E_RANGE", n, t.name + "." + f.name + " is outside its limits", limitsText(f), ctx.gotText(n));
    return ctx.errors.size() == before;
}

} // namespace ohao::reflect
