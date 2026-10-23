// The JSON reader behind scene files and API params: one pass over the text with an
// explicit stack, building reflect::JsonDocument directly. Numbers keep their raw token
// (decoded later straight to float), strings without escapes are views into the input,
// and UTF-8 is validated up front with simdjson's SIMD validator. Strict RFC 8259:
// no comments, no trailing commas, no leading zeros, no lone surrogates.
#include "ecs/json_parse.hpp"

#include <simdjson.h>

#include <emmintrin.h>

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ohao::ecs {

namespace {

using reflect::Diagnostic;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;

constexpr std::size_t kPadding = 32;     // NUL bytes after the text: look-aheads and 16-byte loads stop on them
constexpr std::size_t kMaxDepth = 1024;  // nesting limit (a hostile document cannot exhaust the stack)

struct Frame {
    uint32_t node;   // the container's node index
    uint32_t mark;   // where its children start in the pending list
    bool object;
};

// Reusable working memory: kept warm by JsonParser between parses.
struct Scratch {
    std::vector<uint32_t> pending;   // child indices of the open containers
    std::vector<Frame> frames;
};

[[nodiscard]] inline bool isWs(char c) noexcept { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
[[nodiscard]] inline bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] inline bool isDelimiter(char c) noexcept { return isWs(c) || c == ',' || c == ']' || c == '}' || c == '\0'; }

// 16 bytes at a time: pretty files have whitespace runs of every length (", ", a newline
// and the indentation), which a byte loop mispredicts once per run. The padding after the
// text keeps every load in bounds, and its NUL bytes stop the scan.
[[nodiscard]] inline const char* skipWs(const char* p) noexcept {
    if (!isWs(*p)) return p;   // compact text, and most token boundaries in pretty text
    for (;;) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\n'))),
                                        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\r')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\t'))));
        const auto other = static_cast<unsigned>(~_mm_movemask_epi8(ws)) & 0xFFFFu;
        if (other) return p + std::countr_zero(other);
        p += 16;
    }
}

// Offset of the first byte that is not valid UTF-8 (only called once the fast check failed).
std::size_t firstInvalidUtf8(const unsigned char* s, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        std::size_t len;
        uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
        else return i;
        if (i + len > n) return i;
        for (std::size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0u) != 0x80u) return i;
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            return i;
        i += len;
    }
    return n;
}

class Parser {
public:
    Parser(const char* base, std::size_t size, std::string& strings, JsonDocument& doc, Scratch& scratch)
        : m_base(base), m_end(base + size), m_strings(strings), m_doc(doc), m_s(scratch) {}

    // Fills the document; on failure returns the E_PARSE diagnostic (offset set).
    std::optional<Diagnostic> run() {
        m_s.pending.clear();
        m_s.frames.clear();
        const char* p = skipWs(m_base);
        if (p == m_end) return fail("the document is empty", p);
        std::string_view key;
        for (;;) {
            // A value starts at p.
            if (p == m_end) return fail("unexpected end of the document; a value is missing", p);
            auto idx = static_cast<uint32_t>(m_doc.nodes.size());
            JsonNode& n = m_doc.nodes.emplace_back();
            n.offset = static_cast<uint32_t>(p - m_base);
            n.key = key;
            key = {};
            bool complete = true;
            switch (*p) {
            case '{':
            case '[': {
                const bool object = *p == '{';
                if (m_s.frames.size() >= kMaxDepth) return fail("the document nests deeper than 1024 levels", p);
                m_s.frames.push_back(Frame{idx, static_cast<uint32_t>(m_s.pending.size()), object});
                p = skipWs(p + 1);
                if (*p == (object ? '}' : ']')) {
                    ++p;
                    close();
                } else {
                    complete = false;
                    if (object && !readKey(p, key)) return error();
                }
                break;
            }
            case '"': {
                std::string_view s;
                if (!readString(p, s)) return error();
                n.type = JsonType::String;
                n.text = s;
                break;
            }
            case 't':
                if (!literal(p, "true", 4)) return error();
                n.type = JsonType::Bool;
                n.boolean = true;
                break;
            case 'f':
                if (!literal(p, "false", 5)) return error();
                n.type = JsonType::Bool;
                break;
            case 'n':
                if (!literal(p, "null", 4)) return error();
                break;
            default: {
                std::string_view tok;
                if (!readNumber(p, tok)) return error();
                n.type = JsonType::Number;
                n.text = tok;
            }
            }
            if (!complete) continue;

            // After a complete value: close containers, or move to the next member.
            for (;;) {
                if (m_s.frames.empty()) {
                    p = skipWs(p);
                    if (p != m_end) return fail("unexpected content after the document", p);
                    return std::nullopt;
                }
                m_s.pending.push_back(idx);
                const Frame& f = m_s.frames.back();
                p = skipWs(p);
                if (*p == ',') {
                    p = skipWs(p + 1);
                    if (f.object && !readKey(p, key)) return error();
                    break;
                }
                if (*p == (f.object ? '}' : ']')) {
                    ++p;
                    idx = f.node;
                    close();
                    continue;
                }
                if (p == m_end) return fail(f.object ? "unexpected end of the document; '}' is missing"
                                                     : "unexpected end of the document; ']' is missing",
                                            p);
                return fail(f.object ? "expected ',' or '}' after an object member"
                                     : "expected ',' or ']' after an array element",
                            p);
            }
        }
    }

private:
    void close() {
        const Frame f = m_s.frames.back();
        m_s.frames.pop_back();
        JsonNode& n = m_doc.nodes[f.node];
        n.type = f.object ? JsonType::Object : JsonType::Array;
        n.first = static_cast<uint32_t>(m_doc.children.size());
        n.count = static_cast<uint32_t>(m_s.pending.size() - f.mark);
        m_doc.children.insert(m_doc.children.end(), m_s.pending.begin() + f.mark, m_s.pending.end());
        m_s.pending.resize(f.mark);
    }

    // `"name"`, then ':' -- leaves p on the member's value.
    bool readKey(const char*& p, std::string_view& key) {
        if (*p != '"') {
            setError(p == m_end ? "unexpected end of the document; a member name is missing"
                                : "expected a member name in double quotes",
                     p);
            return false;
        }
        if (!readString(p, key)) return false;
        p = skipWs(p);
        if (*p != ':') {
            setError("expected ':' after the member name", p);
            return false;
        }
        p = skipWs(p + 1);
        return true;
    }

    // p is on the opening quote; leaves p after the closing one.
    bool readString(const char*& p, std::string_view& out) {
        const char* s = p + 1;
        const char* q = s;
        for (;;) {
            const auto c = static_cast<unsigned char>(*q);
            if (c == '"') {
                out = std::string_view(s, static_cast<std::size_t>(q - s));
                p = q + 1;
                return true;
            }
            if (c == '\\') return readEscaped(p, s, q, out);
            if (c < 0x20) {
                setError(q == m_end ? "unterminated string" : "a control character inside a string must be escaped",
                         q == m_end ? p : q);
                return false;
            }
            ++q;
        }
    }

    // The slow path: copies the string, decoding escapes, into the string arena (reserved
    // to the input size, so views into it stay valid).
    bool readEscaped(const char*& p, const char* s, const char* q, std::string_view& out) {
        const std::size_t start = m_strings.size();
        m_strings.append(s, q);
        for (;;) {
            const auto c = static_cast<unsigned char>(*q);
            if (c == '"') break;
            if (c < 0x20) {
                setError(q == m_end ? "unterminated string" : "a control character inside a string must be escaped",
                         q == m_end ? p : q);
                return false;
            }
            if (c != '\\') {
                m_strings.push_back(static_cast<char>(c));
                ++q;
                continue;
            }
            const char e = q[1];
            switch (e) {
            case '"': m_strings.push_back('"'); break;
            case '\\': m_strings.push_back('\\'); break;
            case '/': m_strings.push_back('/'); break;
            case 'b': m_strings.push_back('\b'); break;
            case 'f': m_strings.push_back('\f'); break;
            case 'n': m_strings.push_back('\n'); break;
            case 'r': m_strings.push_back('\r'); break;
            case 't': m_strings.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(q + 2, cp)) return escapeError(q);
                q += 4;
                if (cp >= 0xDC00 && cp <= 0xDFFF) return escapeError(q - 4);   // lone low surrogate
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo = 0;
                    if (q[2] != '\\' || q[3] != 'u' || !hex4(q + 4, lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return escapeError(q - 4);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    q += 6;
                }
                appendUtf8(cp);
                break;
            }
            default: return escapeError(q);
            }
            q += 2;
        }
        out = std::string_view(m_strings.data() + start, m_strings.size() - start);
        p = q + 1;
        return true;
    }

    static bool hex4(const char* h, uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = h[i];
            uint32_t d;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        return true;
    }

    void appendUtf8(uint32_t cp) {
        if (cp < 0x80) {
            m_strings.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            m_strings.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            m_strings.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            m_strings.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            m_strings.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            m_strings.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            m_strings.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            m_strings.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            m_strings.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            m_strings.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool escapeError(const char* at) {
        setError("invalid escape sequence in a string (\\uD800-\\uDFFF must form a surrogate pair)", at);
        return false;
    }

    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? followed by a delimiter.
    bool readNumber(const char*& p, std::string_view& tok) {
        const char* s = p;
        const char* q = p;
        bool ok = true;
        if (*q == '-') ++q;
        if (*q == '0') ++q;
        else if (isDigit(*q)) while (isDigit(*q)) ++q;
        else ok = false;
        if (ok && *q == '.') {
            ++q;
            ok = isDigit(*q);
            while (isDigit(*q)) ++q;
        }
        if (ok && (*q == 'e' || *q == 'E')) {
            ++q;
            if (*q == '+' || *q == '-') ++q;
            ok = isDigit(*q);
            while (isDigit(*q)) ++q;
        }
        if (ok && isDelimiter(*q) && q != s) {
            tok = std::string_view(s, static_cast<std::size_t>(q - s));
            p = q;
            return true;
        }
        const char* e = q;
        while (!isDelimiter(*e) && *e != '"' && *e != ':') ++e;
        if (e == s) e = s + 1;
        setError(isDigit(*s) || *s == '-' || *s == '.' || *s == '+'
                     ? "invalid number '" + std::string(s, e) + "'"
                     : "unexpected character '" + std::string(s, e) + "'; expected a value",
                 s);
        return false;
    }

    bool literal(const char*& p, const char* word, std::size_t len) {
        if (std::memcmp(p, word, len) == 0 && isDelimiter(p[len])) {
            p += len;
            return true;
        }
        const char* e = p;
        while (!isDelimiter(*e) && *e != '"' && *e != ':') ++e;
        setError("unexpected '" + std::string(p, e) + "'; expected a value", p);
        return false;
    }

    void setError(std::string message, const char* at) {
        m_error = std::move(message);
        m_errorAt = at;
    }
    std::optional<Diagnostic> error() { return fail(std::move(m_error), m_errorAt); }
    std::optional<Diagnostic> fail(std::string message, const char* at) {
        Diagnostic d;
        d.code = "E_PARSE";
        d.offset = static_cast<uint32_t>(at - m_base);
        d.message = std::move(message);
        reflect::fillLineColumns(std::span<Diagnostic>(&d, 1), std::string_view(m_base, m_end - m_base));
        return d;
    }

    const char* m_base;
    const char* m_end;
    std::string& m_strings;
    JsonDocument& m_doc;
    Scratch& m_s;
    std::string m_error;
    const char* m_errorAt{nullptr};
};

// Parses `input` (followed by kPadding NUL bytes) into `doc`, whose capacity is kept.
std::optional<Diagnostic> parseInto(std::string_view input, std::string& strings, JsonDocument& doc, Scratch& s) {
    doc.nodes.clear();
    doc.children.clear();
    doc.source = input;
    if (!simdjson::validate_utf8(input.data(), input.size())) {
        Diagnostic d;
        d.code = "E_PARSE";
        d.offset = static_cast<uint32_t>(firstInvalidUtf8(reinterpret_cast<const unsigned char*>(input.data()), input.size()));
        d.message = "the text is not valid UTF-8";
        reflect::fillLineColumns(std::span<Diagnostic>(&d, 1), input);
        return d;
    }
    strings.clear();
    strings.reserve(input.size());
    doc.nodes.reserve(input.size() / 8 + 16);
    doc.children.reserve(input.size() / 8 + 16);
    return Parser(input.data(), input.size(), strings, doc, s).run();
}

// The bytes every view in a parseJson() document points into.
struct Backing {
    std::string input;
    std::string strings;
};

} // namespace

bool validUtf8(std::string_view text) noexcept { return simdjson::validate_utf8(text.data(), text.size()); }

Result<JsonDocument, Diagnostic> parseJson(std::string_view text) {
    using R = Result<JsonDocument, Diagnostic>;
    auto backing = std::make_shared<Backing>();
    backing->input.reserve(text.size() + kPadding);
    backing->input.assign(text);
    backing->input.append(kPadding, '\0');
    JsonDocument doc;
    Scratch scratch;
    if (auto err = parseInto(std::string_view(backing->input.data(), text.size()), backing->strings, doc, scratch))
        return R::err(std::move(*err));
    doc.storage = std::move(backing);
    return R::ok(std::move(doc));
}

struct JsonParser::Impl {
    std::string input;     // the text plus padding
    std::string strings;   // unescaped strings
    JsonDocument doc;
    Scratch scratch;
};

JsonParser::JsonParser() : m(std::make_unique<Impl>()) {}
JsonParser::~JsonParser() = default;

Result<const JsonDocument*, Diagnostic> JsonParser::parse(std::string_view text) {
    using R = Result<const JsonDocument*, Diagnostic>;
    m->input.reserve(text.size() + kPadding);
    m->input.assign(text);
    m->input.append(kPadding, '\0');
    if (auto err = parseInto(std::string_view(m->input.data(), text.size()), m->strings, m->doc, m->scratch))
        return R::err(std::move(*err));
    return R::ok(&m->doc);
}

} // namespace ohao::ecs
