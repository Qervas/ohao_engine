#include "reflect/json_cursor.hpp"

#include <emmintrin.h>
#include <fast_float/fast_float.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <system_error>

namespace ohao::reflect {

namespace {

[[nodiscard]] inline bool isWs(char c) noexcept { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
[[nodiscard]] inline bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] inline bool isDelimiter(char c) noexcept {
    return isWs(c) || c == ',' || c == ']' || c == '}' || c == '\0';
}

bool hex4(const char* h, uint32_t& out) {
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

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

} // namespace

JsonCursor::JsonCursor(const char* text, std::size_t size) : m_base(text), m_p(text), m_end(text + size) {}

// As the tree parser: 16 bytes at a time once inside a run of whitespace.
void JsonCursor::skipWsRun() {
    for (;;) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_p));
        const __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\n'))),
                                        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\r')), _mm_cmpeq_epi8(v, _mm_set1_epi8('\t'))));
        const auto other = static_cast<unsigned>(~_mm_movemask_epi8(ws)) & 0xFFFFu;
        if (other) {
            m_p += std::countr_zero(other);
            return;
        }
        m_p += 16;
    }
}

bool JsonCursor::string(std::string_view& out) {
    if (m_failed) return false;
    skipWs();
    if (*m_p != '"') return fail();
    const char* s = m_p + 1;
    const char* q = s;
    for (;;) {
        const auto c = static_cast<unsigned char>(*q);
        if (c == '"') {
            out = std::string_view(s, static_cast<std::size_t>(q - s));
            m_p = q + 1;
            return true;
        }
        if (c == '\\') break;
        if (c < 0x20) return fail();
        ++q;
    }
    // Escapes: decoded exactly as the tree parser decodes them.
    m_scratch.assign(s, q);
    for (;;) {
        const auto c = static_cast<unsigned char>(*q);
        if (c == '"') break;
        if (c < 0x20) return fail();
        if (c != '\\') {
            m_scratch.push_back(static_cast<char>(c));
            ++q;
            continue;
        }
        switch (q[1]) {
        case '"': m_scratch.push_back('"'); break;
        case '\\': m_scratch.push_back('\\'); break;
        case '/': m_scratch.push_back('/'); break;
        case 'b': m_scratch.push_back('\b'); break;
        case 'f': m_scratch.push_back('\f'); break;
        case 'n': m_scratch.push_back('\n'); break;
        case 'r': m_scratch.push_back('\r'); break;
        case 't': m_scratch.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!hex4(q + 2, cp)) return fail();
            q += 4;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return fail();   // lone low surrogate
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;
                if (q[2] != '\\' || q[3] != 'u' || !hex4(q + 4, lo) || lo < 0xDC00 || lo > 0xDFFF) return fail();
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                q += 6;
            }
            appendUtf8(m_scratch, cp);
            break;
        }
        default: return fail();
        }
        q += 2;
    }
    out = m_scratch;
    m_p = q + 1;
    return true;
}

// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? followed by a delimiter.
bool JsonCursor::number(std::string_view& token) {
    if (m_failed) return false;
    skipWs();
    const char* s = m_p;
    const char* q = m_p;
    if (*q == '-') ++q;
    if (*q == '0') ++q;
    else if (isDigit(*q)) while (isDigit(*q)) ++q;
    else return fail();
    if (*q == '.') {
        ++q;
        if (!isDigit(*q)) return fail();
        while (isDigit(*q)) ++q;
    }
    if (*q == 'e' || *q == 'E') {
        ++q;
        if (*q == '+' || *q == '-') ++q;
        if (!isDigit(*q)) return fail();
        while (isDigit(*q)) ++q;
    }
    if (!isDelimiter(*q)) return fail();
    token = std::string_view(s, static_cast<std::size_t>(q - s));
    m_p = q;
    return true;
}

bool JsonCursor::literal(const char* word, std::size_t len) {
    if (m_failed) return false;
    skipWs();
    if (std::memcmp(m_p, word, len) != 0 || !isDelimiter(m_p[len])) return fail();
    m_p += len;
    return true;
}

bool JsonCursor::boolean(bool& out) {
    if (m_failed) return false;
    skipWs();
    if (*m_p == 't') {
        out = true;
        return literal("true", 4);
    }
    out = false;
    return literal("false", 5);
}

bool JsonCursor::null() { return literal("null", 4); }

// Grammar and fit in one pass (validation, the value unused): the decision readFloat
// makes with floatFitsQuick, then a conversion only when that is not certain.
bool JsonCursor::numberFits() {
    if (m_failed) return false;
    skipWs();
    const char* const s = m_p;
    const char* q = s;
    if (*q == '-') ++q;
    int lead = 0;   // decimal exponent of the leading nonzero digit
    bool nonzero = false;
    if (*q == '0') {
        ++q;
    } else if (isDigit(*q)) {
        const char* d = q;
        while (isDigit(*q)) ++q;
        nonzero = true;
        lead = static_cast<int>(q - d) - 1;
    } else {
        return fail();
    }
    if (*q == '.') {
        ++q;
        if (!isDigit(*q)) return fail();
        if (!nonzero) {
            const char* z = q;
            while (*q == '0') ++q;
            if (isDigit(*q)) {
                nonzero = true;
                lead = -static_cast<int>(q - z) - 1;
            }
        }
        while (isDigit(*q)) ++q;
    }
    int exp = 0;
    bool longExp = false;
    if (*q == 'e' || *q == 'E') {
        ++q;
        const bool neg = *q == '-';
        if (*q == '+' || *q == '-') ++q;
        if (!isDigit(*q)) return fail();
        const char* d = q;
        while (isDigit(*q)) {
            if (q - d < 4) exp = exp * 10 + (*q - '0');
            ++q;
        }
        longExp = q - d > 3;
        if (neg) exp = -exp;
    }
    if (!isDelimiter(*q)) return fail();
    m_p = q;
    if (!nonzero) return true;   // zero, in any spelling, fits
    if (!longExp && lead + exp >= -44 && lead + exp <= 37) return true;
    float v;   // near the ends of float's range: convert, as readFloat does
    const auto r = fast_float::from_chars(s, q, v);
    if (r.ec == std::errc::invalid_argument || r.ec == std::errc::result_out_of_range || !std::isfinite(v) || r.ptr != q)
        return fail();
    if (v == 0.0f)
        for (const char* p = s; p < q && *p != 'e' && *p != 'E'; ++p)
            if (*p >= '1' && *p <= '9') return fail();
    return true;
}

bool JsonCursor::numberAsFloat(float& out) {
    if (m_failed) return false;
    skipWs();
    // JSON grammar (no leading zeros, '+', bare '.' or exponent), correctly rounded
    // straight to float, as the tree decoder's readFloat converts (never via double).
    const auto r = fast_float::from_chars(m_p, m_end, out, fast_float::chars_format::json);
    if (r.ec == std::errc::invalid_argument || !isDelimiter(*r.ptr)) return fail();
    if (r.ec == std::errc::result_out_of_range || !std::isfinite(out)) return fail();
    if (out == 0.0f)   // a nonzero literal that underflowed to zero does not fit
        for (const char* p = m_p; p < r.ptr && *p != 'e' && *p != 'E'; ++p)
            if (*p >= '1' && *p <= '9') return fail();
    m_p = r.ptr;
    return true;
}

bool JsonCursor::atEnd() {
    if (m_failed) return false;
    skipWs();
    return m_p == m_end;
}

} // namespace ohao::reflect
