#include "reflect/json_writer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>

namespace ohao::reflect {

namespace {

// Bytes a JSON string must escape: '"', '\\' and control characters.
constexpr std::array<bool, 256> kNeedsEscape = [] {
    std::array<bool, 256> t{};
    for (int c = 0; c < 0x20; ++c) t[c] = true;
    t['"'] = true;
    t['\\'] = true;
    return t;
}();

// Index of the closing quote of the string token that starts at s[i] == '"'.
std::size_t stringEnd(std::string_view s, std::size_t i) {
    ++i;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') return i;
        i += (c == '\\') ? 2 : 1;
    }
    return s.size() - 1;
}

} // namespace

JsonWriter::JsonWriter(std::size_t reserveBytes, JsonLayout layout, int width)
    : m_layout(layout), m_width(static_cast<uint32_t>(width)) {
    m_out.reserve(reserveBytes);
}

// Compact: a comma is due exactly when the previous token was a value or a closed
// container. Pretty: the frame stack knows; see the class comment.
void JsonWriter::beforeValue() {
    if (m_layout == JsonLayout::Compact) {
        if (m_needComma) m_out.push_back(',');
        return;
    }
    if (m_afterKey) { m_afterKey = false; return; }
    if (m_frames.empty()) { m_nextColumn = 0; return; }
    if (m_frames.back().hasChildren) resolvePending(true);   // the previous child is followed by a comma
    Frame& f = m_frames.back();
    // Already longer than a line can hold: it will break, so break it now and write the
    // rest of its children directly in broken form.
    if (!f.broken && f.column + (static_cast<uint32_t>(m_out.size()) - f.start) + 1u > m_width) breakOpenFrames();
    const uint32_t indent = 2u * (f.depth + 1u);
    if (f.broken) {
        if (f.hasChildren) m_out.push_back(',');
        f.hasChildren = true;
        m_out.push_back('\n');
        m_out.append(indent, ' ');
    } else if (f.hasChildren) {
        m_seps.push_back(static_cast<uint32_t>(m_out.size()));
        m_out.append(", ", 2);
    } else {
        f.hasChildren = true;
        if (f.object) m_out.push_back(' ');
    }
    m_nextColumn = indent;
}

void JsonWriter::openContainer(bool object) {
    beforeValue();
    if (m_layout == JsonLayout::Compact) {
        m_out.push_back(object ? '{' : '[');
        m_needComma = false;
        return;
    }
    const bool root = m_frames.empty();   // the root always breaks
    m_frames.push_back(Frame{static_cast<uint32_t>(m_out.size()), m_nextColumn,
                             static_cast<uint32_t>(m_seps.size()), 0,
                             static_cast<uint16_t>(m_frames.size()), object, false, root});
    m_out.push_back(object ? '{' : '[');
}

void JsonWriter::closeContainer(bool object) {
    if (m_layout == JsonLayout::Compact) {
        m_out.push_back(object ? '}' : ']');
        m_needComma = true;
        return;
    }
    resolvePending(false);   // the last child is followed by the closing bracket
    Frame f = m_frames.back();
    m_frames.pop_back();
    if (f.broken) {
        if (f.hasChildren) {
            m_out.push_back('\n');
            m_out.append(2u * f.depth, ' ');
        }
        m_out.push_back(object ? '}' : ']');
        return;   // its parent is broken too (breakOpenFrames keeps that invariant)
    }
    if (f.hasChildren && f.object) m_out.append(" }", 2);
    else m_out.push_back(object ? '}' : ']');
    f.end = static_cast<uint32_t>(m_out.size() - 1);
    // Whether it fits depends on a trailing comma we only learn from the next token.
    m_pending = f;
    m_hasPending = true;
}

// Decides the layout of the last closed one-line container. It still sits at the tail of
// the buffer: nothing has been written after it.
void JsonWriter::resolvePending(bool trailingComma) {
    if (!m_hasPending) return;
    m_hasPending = false;
    const Frame& p = m_pending;
    const uint32_t oneLine = p.end - p.start + 1;
    const bool fits = !p.hasChildren || p.column + oneLine + (trailingComma ? 1u : 0u) <= m_width;
    if (fits) {
        m_seps.resize(p.sepBegin);
        return;
    }
    breakContainer(p);
    m_seps.resize(p.sepBegin);
    breakOpenFrames();   // a broken child breaks its parent, and so on up
}

// Breaks the open frames that are still on one line, innermost first, up to the first
// broken one. Innermost first: an inner frame's content is the tail of its parent's, so
// breaking it never moves anything the outer frames still need (their separators lie
// before it), and broken frames need no positions at all.
void JsonWriter::breakOpenFrames() {
    for (std::size_t i = m_frames.size(); i-- > 0 && !m_frames[i].broken;) {
        Frame& f = m_frames[i];
        const uint32_t indent = 2u * (f.depth + 1u);
        if (f.hasChildren) {
            const uint32_t contentBegin = f.start + (f.object ? 2u : 1u);   // after "{ " / "["
            m_scratch.assign(m_out.data() + contentBegin, m_out.size() - contentBegin);
            m_out.resize(f.start + 1);
            std::size_t pos = 0;
            for (std::size_t k = f.sepBegin; k < m_seps.size(); ++k) {
                const std::size_t sep = m_seps[k] - contentBegin;
                m_out.push_back('\n');
                m_out.append(indent, ' ');
                m_out.append(m_scratch, pos, sep - pos);
                m_out.push_back(',');
                pos = sep + 2;
            }
            m_out.push_back('\n');
            m_out.append(indent, ' ');
            m_out.append(m_scratch, pos, std::string::npos);
        }
        m_seps.resize(f.sepBegin);
        f.broken = true;
    }
}

// Rewrites the container at the tail from its one-line form to one child per line. Its
// children are already in their final form (broken children carry absolute indents).
void JsonWriter::breakContainer(const Frame& f) {
    const uint32_t pad = f.object ? 2u : 1u;   // "{ " / " }" versus "[" / "]"
    const uint32_t contentBegin = f.start + pad;
    const uint32_t contentEnd = static_cast<uint32_t>(m_out.size()) - pad;
    m_scratch.assign(m_out.data() + contentBegin, contentEnd - contentBegin);
    m_out.resize(f.start + 1);
    const std::size_t indent = 2u * (f.depth + 1u);
    std::size_t pos = 0;
    for (std::size_t k = f.sepBegin; k < m_seps.size(); ++k) {
        const std::size_t sep = m_seps[k] - contentBegin;
        m_out.push_back('\n');
        m_out.append(indent, ' ');
        m_out.append(m_scratch, pos, sep - pos);
        m_out.push_back(',');
        pos = sep + 2;
    }
    m_out.push_back('\n');
    m_out.append(indent, ' ');
    m_out.append(m_scratch, pos, std::string::npos);
    m_out.push_back('\n');
    m_out.append(indent - 2, ' ');
    m_out.push_back(f.object ? '}' : ']');
}

void JsonWriter::beginObject() { openContainer(true); }
void JsonWriter::endObject() { closeContainer(true); }
void JsonWriter::beginArray() { openContainer(false); }
void JsonWriter::endArray() { closeContainer(false); }

void JsonWriter::key(std::string_view k) {
    beforeValue();
    if (m_layout == JsonLayout::Compact) {
        appendString(k);
        m_out.push_back(':');
        m_needComma = false;
        return;
    }
    const std::size_t start = m_out.size();
    appendString(k);
    m_out.append(": ", 2);
    m_nextColumn += static_cast<uint32_t>(m_out.size() - start);
    m_afterKey = true;
}

void JsonWriter::null() { beforeValue(); m_out.append("null", 4); m_needComma = true; }

void JsonWriter::boolean(bool v) {
    beforeValue();
    if (v) m_out.append("true", 4);
    else m_out.append("false", 5);
    m_needComma = true;
}

void JsonWriter::int64(int64_t v) {
    beforeValue();
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, r.ptr);
    m_needComma = true;
}

void JsonWriter::uint64(uint64_t v) {
    beforeValue();
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, r.ptr);
    m_needComma = true;
}

void JsonWriter::float32(float v) {
    if (!std::isfinite(v)) {
        fail("non-finite float");
        null();
        return;
    }
    beforeValue();
    char buf[32];
    m_out.append(buf, formatFloat32(v, buf));
    m_needComma = true;
}

char* formatFloat32(float v, char* buf) {
    // Fast path for whole numbers below 10^4, common in scenes (0, 1, scales, unit
    // quaternions). It prints exactly what to_chars does: shortest form picks fixed
    // notation whenever it is not longer than scientific, and for |v| < 10^4 fixed has
    // at most 4 digits while scientific needs at least 5 characters ("1e+03").
    if (std::fabs(v) < 10000.0f && v == static_cast<float>(static_cast<int32_t>(v))) {
        const auto i = static_cast<int32_t>(v);
        if (i == 0 && std::signbit(v)) {
            buf[0] = '-';
            buf[1] = '0';
            return buf + 2;
        }
        return std::to_chars(buf, buf + 32, i).ptr;
    }
    // Scenes repeat values (palette colors, sizes, a uniform scale written three times):
    // a small direct-mapped memo per thread returns a repeat's text without recomputing
    // it. Pure memoization of this function: the bytes are the same either way. A text
    // is at most 16 bytes (sign, 9 digits, point and exponent).
    struct Entry {
        uint32_t bits;
        uint32_t len;   // 0: empty
        char text[24];
    };
    thread_local Entry memo[512];
    const uint32_t bits = std::bit_cast<uint32_t>(v);
    Entry& e = memo[(bits * 0x9E3779B1u) >> 23];
    if (e.len != 0 && e.bits == bits) {
        std::memcpy(buf, e.text, 16);
        return buf + e.len;
    }
    char* p = buf;
    if (std::signbit(v)) *p++ = '-';
    const float a = std::fabs(v);
    const ShortestDecimal d = shortestDecimal(a);
    char* const end = writeShortestOf(p, d.digits, d.exponent, a);
    e.bits = bits;
    e.len = static_cast<uint32_t>(end - buf);
    std::memcpy(e.text, buf, 16);
    return end;
}

// ---------------------------------------------------------------------------------------
// Shortest round-trip digits of a float: Schubfach (R. Giulietti, "The Schubfach way to
// render doubles", 2020), binary32 variant as in A. Bolz's C++ port. One 64x32-bit
// multiply per interval bound and no digit-by-digit loop. The result is the decimal
// to_chars picks: the shortest digit string that reads back as the same float, the one
// closest to it among those, ties to even. reflect_tests checks it against std::to_chars
// (a sample on every run; all 2^32 bit patterns with --exhaustive-floats).

namespace {

// 10^k as a 64-bit significand g = ceil(10^k / 2^r), r = floor(log2 10^k) - 63, for
// k in [-31, 45]: 2^63 <= g < 2^64 overestimates 10^k * 2^-r by less than one unit.
// Generated with exact rational arithmetic (Python Fraction: g = ceil(10^k / 2^r)); the
// to_chars comparisons in reflect_tests cover every entry.
constexpr int kPow10Min = -31;
constexpr uint64_t kPow10[] = {
    0x81CEB32C4B43FCF5u, 0xA2425FF75E14FC32u, 0xCAD2F7F5359A3B3Fu, 0xFD87B5F28300CA0Eu,
    0x9E74D1B791E07E49u, 0xC612062576589DDBu, 0xF79687AED3EEC552u, 0x9ABE14CD44753B53u,
    0xC16D9A0095928A28u, 0xF1C90080BAF72CB2u, 0x971DA05074DA7BEFu, 0xBCE5086492111AEBu,
    0xEC1E4A7DB69561A6u, 0x9392EE8E921D5D08u, 0xB877AA3236A4B44Au, 0xE69594BEC44DE15Cu,
    0x901D7CF73AB0ACDAu, 0xB424DC35095CD810u, 0xE12E13424BB40E14u, 0x8CBCCC096F5088CCu,
    0xAFEBFF0BCB24AAFFu, 0xDBE6FECEBDEDD5BFu, 0x89705F4136B4A598u, 0xABCC77118461CEFDu,
    0xD6BF94D5E57A42BDu, 0x8637BD05AF6C69B6u, 0xA7C5AC471B478424u, 0xD1B71758E219652Cu,
    0x83126E978D4FDF3Cu, 0xA3D70A3D70A3D70Bu, 0xCCCCCCCCCCCCCCCDu, 0x8000000000000000u,
    0xA000000000000000u, 0xC800000000000000u, 0xFA00000000000000u, 0x9C40000000000000u,
    0xC350000000000000u, 0xF424000000000000u, 0x9896800000000000u, 0xBEBC200000000000u,
    0xEE6B280000000000u, 0x9502F90000000000u, 0xBA43B74000000000u, 0xE8D4A51000000000u,
    0x9184E72A00000000u, 0xB5E620F480000000u, 0xE35FA931A0000000u, 0x8E1BC9BF04000000u,
    0xB1A2BC2EC5000000u, 0xDE0B6B3A76400000u, 0x8AC7230489E80000u, 0xAD78EBC5AC620000u,
    0xD8D726B7177A8000u, 0x878678326EAC9000u, 0xA968163F0A57B400u, 0xD3C21BCECCEDA100u,
    0x84595161401484A0u, 0xA56FA5B99019A5C8u, 0xCECB8F27F4200F3Au, 0x813F3978F8940985u,
    0xA18F07D736B90BE6u, 0xC9F2C9CD04674EDFu, 0xFC6F7C4045812297u, 0x9DC5ADA82B70B59Eu,
    0xC5371912364CE306u, 0xF684DF56C3E01BC7u, 0x9A130B963A6C115Du, 0xC097CE7BC90715B4u,
    0xF0BDC21ABB48DB21u, 0x96769950B50D88F5u, 0xBC143FA4E250EB32u, 0xEB194F8E1AE525FEu,
    0x92EFD1B8D0CF37BFu, 0xB7ABC627050305AEu, 0xE596B7B0C643C71Au, 0x8F7E32CE7BEA5C70u,
    0xB35DBF821AE4F38Cu,
};
static_assert(std::size(kPow10) == 45 - kPow10Min + 1);

inline int floorLog2Pow10(int e) { return (e * 1741647) >> 19; }
inline int floorLog10Pow2(int e) { return (e * 1262611) >> 22; }
inline int floorLog10ThreeQuartersPow2(int e) { return (e * 1262611 - 524031) >> 22; }

// floor(g * cp / 2^64), rounded to odd (the paper's vb with its sticky bit).
inline uint32_t roundToOdd(uint64_t g, uint32_t cp) {
    const uint64_t lo = (g & 0xFFFFFFFFu) * cp;
    const uint64_t hi = (g >> 32) * cp;
    const uint64_t mid = hi + (lo >> 32);
    const auto y1 = static_cast<uint32_t>(mid >> 32);
    const auto y0 = static_cast<uint32_t>(mid);
    return y1 | (y0 > 1u ? 1u : 0u);
}

} // namespace

ShortestDecimal shortestDecimal(float v) {
    const uint32_t bitsV = std::bit_cast<uint32_t>(v);
    const uint32_t ieeeMantissa = bitsV & 0x7FFFFFu;
    const uint32_t ieeeExponent = (bitsV >> 23) & 0xFFu;
    if (ieeeExponent == 0 && ieeeMantissa == 0) return {0, 0};
    uint32_t c;
    int q;
    if (ieeeExponent != 0) {
        c = (1u << 23) | ieeeMantissa;
        q = static_cast<int>(ieeeExponent) - 150;
    } else {
        c = ieeeMantissa;
        q = -149;
    }
    const bool isEven = (c % 2) == 0;
    const bool lowerBoundaryIsCloser = ieeeMantissa == 0 && ieeeExponent > 1;
    const uint32_t cbl = 4 * c - 2 + (lowerBoundaryIsCloser ? 1u : 0u);
    const uint32_t cb = 4 * c;
    const uint32_t cbr = 4 * c + 2;
    const int k = lowerBoundaryIsCloser ? floorLog10ThreeQuartersPow2(q) : floorLog10Pow2(q);
    const int h = q + floorLog2Pow10(-k) + 1;
    const uint64_t pow10 = kPow10[-k - kPow10Min];
    const uint32_t vbl = roundToOdd(pow10, cbl << h);
    const uint32_t vb = roundToOdd(pow10, cb << h);
    const uint32_t vbr = roundToOdd(pow10, cbr << h);
    const uint32_t lower = vbl + (isEven ? 0u : 1u);
    const uint32_t upper = vbr - (isEven ? 0u : 1u);

    uint32_t digits;
    int exponent;
    const uint32_t s = vb / 4;
    bool done = false;
    if (s >= 10) {
        const uint32_t sp = s / 10;
        const bool upInside = lower <= 40 * sp;
        const bool wpInside = 40 * sp + 40 <= upper;
        if (upInside != wpInside) {
            digits = sp + (wpInside ? 1u : 0u);
            exponent = k + 1;
            done = true;
        }
    }
    if (!done) {
        const bool uInside = lower <= 4 * s;
        const bool wInside = 4 * s + 4 <= upper;
        if (uInside != wInside) {
            digits = s + (wInside ? 1u : 0u);
        } else {
            const uint32_t mid = 4 * s + 2;
            const bool roundUp = vb > mid || (vb == mid && (s & 1u) != 0);
            digits = s + (roundUp ? 1u : 0u);
        }
        exponent = k;
    }
    // Trailing zeros (up to 9 for a float): usually none, so one test; else 8, 4, 2, 1.
    if (digits % 10 == 0) {
        if (digits % 100000000 == 0) { digits /= 100000000; exponent += 8; }
        if (digits % 10000 == 0) { digits /= 10000; exponent += 4; }
        if (digits % 100 == 0) { digits /= 100; exponent += 2; }
        if (digits % 10 == 0) { digits /= 10; exponent += 1; }
    }
    return {digits, exponent};
}

namespace {

constexpr uint32_t kPow10u32[] = {1,      10,      100,      1000,      10000,
                                  100000, 1000000, 10000000, 100000000, 1000000000};

// Decimal digits of x >= 1, without a division loop: from the bit length.
inline int decimalLength(uint32_t x) {
    const int t = ((32 - std::countl_zero(x | 1u)) * 1233) >> 12;   // floor(log10) or one less
    return t + (x >= kPow10u32[t] ? 1 : 0);
}

// Pairs of digits, so a 9-digit number takes 5 steps instead of 9 divisions.
constexpr char kDigitPairs[201] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

// x < 10^9 as exactly 9 digits (leading zeros) at out[0, 9): one division by 10^8, then
// two independent 4-digit halves, so the steps do not wait on each other.
inline void writeNineDigits(char* out, uint32_t x) {
    const uint32_t a = x / 100000000u;
    const uint32_t r = x - a * 100000000u;
    const uint32_t hi = r / 10000u, lo = r - hi * 10000u;
    const uint32_t h1 = hi / 100u, h2 = hi - h1 * 100u;
    const uint32_t l1 = lo / 100u, l2 = lo - l1 * 100u;
    out[0] = static_cast<char>('0' + a);
    std::memcpy(out + 1, kDigitPairs + 2 * h1, 2);
    std::memcpy(out + 3, kDigitPairs + 2 * h2, 2);
    std::memcpy(out + 5, kDigitPairs + 2 * l1, 2);
    std::memcpy(out + 7, kDigitPairs + 2 * l2, 2);
}

} // namespace

// to_chars' choice for the shortest form: fixed notation unless scientific ("d.ddde+XX",
// at least two exponent digits) is strictly shorter.
bool shortestIsFixed(uint32_t digits, int exponent) {
    const int n = decimalLength(digits);
    const int sciExp = exponent + n - 1;
    const int absExp = sciExp < 0 ? -sciExp : sciExp;
    const int sciLen = n + (n > 1 ? 1 : 0) + 2 + (absExp >= 100 ? 3 : 2);
    int fixedLen;
    if (exponent >= 0) fixedLen = n + exponent;
    else if (n + exponent > 0) fixedLen = n + 1;
    else fixedLen = 2 - exponent;
    return fixedLen <= sciLen;
}

char* writeShortest(char* out, uint32_t digits, int exponent) {
    return writeShortestOf(out, digits, exponent, -1.0f);
}

// digits * 10^exponent as to_chars prints the shortest form (see shortestIsFixed).
// `value` (when >= 0) is the float itself: when fixed notation wins and the digits end
// before the decimal point (only integers >= 2^24, below 10^15), to_chars prints the
// float's exact integer value (printf's %.0f), not the digits padded with zeros.
char* writeShortestOf(char* out, uint32_t digits, int exponent, float value) {
    if (digits == 0) {
        *out++ = '0';
        return out;
    }
    const int n = decimalLength(digits);
    const int sciExp = exponent + n - 1;
    const int absExp = sciExp < 0 ? -sciExp : sciExp;
    const int sciLen = n + (n > 1 ? 1 : 0) + 2 + (absExp >= 100 ? 3 : 2);
    int fixedLen;
    if (exponent >= 0) fixedLen = n + exponent;
    else if (n + exponent > 0) fixedLen = n + 1;
    else fixedLen = 2 - exponent;
    if (fixedLen <= sciLen && exponent > 0 && value >= 0.0f)
        return std::to_chars(out, out + 24, static_cast<uint64_t>(value)).ptr;
    // The digits at d[0, n); every copy below is a fixed 16 bytes (inlined, no library
    // call) of which only the first ones count: the output has room (see formatFloat32).
    char nine[32] = {};
    const char* d;
    if (digits < 1000000000u) {
        writeNineDigits(nine, digits);
        d = nine + 9 - n;
    } else {   // ten digits: not produced for floats, kept correct anyway
        const char* const end = std::to_chars(nine, nine + 16, digits).ptr;
        d = end - n;
    }
    if (fixedLen <= sciLen) {
        if (exponent >= 0) {
            std::memcpy(out, d, 16);
            out += n;
            std::memset(out, '0', 16);
            return out + exponent;
        }
        const int whole = n + exponent;   // digits before the point
        if (whole > 0) {
            std::memcpy(out, d, 16);
            out[whole] = '.';
            std::memcpy(out + whole + 1, d + whole, 16);
            return out + n + 1;
        }
        out[0] = '0';
        out[1] = '.';
        std::memset(out + 2, '0', 16);
        out += 2 - whole;
        std::memcpy(out, d, 16);
        return out + n;
    }
    *out++ = d[0];
    if (n > 1) {
        *out++ = '.';
        std::memcpy(out, d + 1, 16);
        out += n - 1;
    }
    *out++ = 'e';
    *out++ = sciExp < 0 ? '-' : '+';
    if (absExp >= 100) *out++ = static_cast<char>('0' + absExp / 100);
    *out++ = static_cast<char>('0' + (absExp / 10) % 10);
    *out++ = static_cast<char>('0' + absExp % 10);
    return out;
}

std::string hex16(uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<std::size_t>(i)] = kDigits[v & 15u];
    return s;
}

void JsonWriter::hex64(uint64_t v) {
    beforeValue();
    static constexpr char kDigits[] = "0123456789abcdef";
    char buf[18];
    buf[0] = '"';
    for (int i = 0; i < 16; ++i) buf[1 + i] = kDigits[(v >> (60 - 4 * i)) & 0xF];
    buf[17] = '"';
    m_out.append(buf, 18);
    m_needComma = true;
}

void JsonWriter::numberToken(std::string_view token) {
    beforeValue();
    m_out.append(token);
    m_needComma = true;
}

void JsonWriter::string(std::string_view s) {
    beforeValue();
    appendString(s);
    m_needComma = true;
}

void JsonWriter::appendString(std::string_view s) { appendJsonString(m_out, s); }

void appendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    const char* p = s.data();
    const char* const end = p + s.size();
    while (p < end) {
        const char* run = p;
        while (p < end && !kNeedsEscape[static_cast<unsigned char>(*p)]) ++p;
        out.append(run, p);
        if (p == end) break;
        const unsigned char c = static_cast<unsigned char>(*p++);
        switch (c) {
        case '"':  out.append("\\\"", 2); break;
        case '\\': out.append("\\\\", 2); break;
        case '\b': out.append("\\b", 2); break;
        case '\f': out.append("\\f", 2); break;
        case '\n': out.append("\\n", 2); break;
        case '\r': out.append("\\r", 2); break;
        case '\t': out.append("\\t", 2); break;
        default: {
            static constexpr char kDigits[] = "0123456789abcdef";
            const char esc[6] = {'\\', 'u', '0', '0', kDigits[c >> 4], kDigits[c & 0xF]};
            out.append(esc, 6);
        }
        }
    }
    out.push_back('"');
}

void JsonWriter::raw(std::string_view compactJson) {
    if (compactJson.empty()) { null(); return; }
    if (m_layout == JsonLayout::Compact) {
        beforeValue();
        m_out.append(compactJson);
        m_needComma = true;
        return;
    }
    rawValue(compactJson, 0);
}

// Re-emits one canonical compact value through the writer so it gets the same layout
// as everything else. Strings and numbers are copied verbatim (already canonical).
std::size_t JsonWriter::rawValue(std::string_view s, std::size_t i) {
    const char c = s[i];
    if (c == '{') {
        beginObject();
        ++i;
        while (i < s.size() && s[i] != '}') {
            const std::size_t e = stringEnd(s, i);
            beforeValue();
            const std::size_t start = m_out.size();
            m_out.append(s.data() + i, e - i + 1);
            m_out.append(": ", 2);
            m_nextColumn += static_cast<uint32_t>(m_out.size() - start);
            m_afterKey = true;
            i = rawValue(s, e + 2);   // past the ':'
            if (i < s.size() && s[i] == ',') ++i;
        }
        endObject();
        return i + 1;
    }
    if (c == '[') {
        beginArray();
        ++i;
        while (i < s.size() && s[i] != ']') {
            i = rawValue(s, i);
            if (i < s.size() && s[i] == ',') ++i;
        }
        endArray();
        return i + 1;
    }
    std::size_t e = i;
    if (c == '"') e = stringEnd(s, i) + 1;
    else while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']') ++e;
    beforeValue();
    m_out.append(s.data() + i, e - i);
    m_needComma = true;
    return e;
}

void JsonWriter::fail(std::string message) {
    if (m_error.empty()) m_error = std::move(message);
}

// ---------------------------------------------------------------------------------------
// prettyPrint: the same layout from finished compact text (used by tools converting
// between the two renderings, and by the tests as an independent implementation).

namespace {

// One entry per container, in document order. The one-line form of a container is its
// compact text plus one space after every ':' and ',' inside it and two padding spaces
// for every non-empty object inside it (itself included): `extraEnd - extraStart`.
struct Container {
    uint32_t start;
    uint32_t end;
    uint32_t extraStart;
    uint32_t extraEnd;
    uint32_t next;   // index of the first container after this one's end
};

struct Layout {
    std::string_view c;
    std::size_t width;
    std::vector<Container> table;
    std::size_t ci{0};   // next container in document order
    std::string out;

    void emitOneLine(std::size_t start, std::size_t end) {
        std::size_t i = start;
        while (i <= end) {
            // Copy the longest run with no structural character.
            std::size_t j = i;
            while (j <= end) {
                const char ch = c[j];
                if (ch == '"' || ch == '{' || ch == '}' || ch == ':' || ch == ',') break;
                ++j;
            }
            out.append(c.data() + i, j - i);
            if (j > end) break;
            const char ch = c[j];
            if (ch == '"') {
                const std::size_t e = stringEnd(c, j);
                out.append(c.data() + j, e - j + 1);
                i = e + 1;
                continue;
            }
            switch (ch) {
            case '{':
                if (c[j + 1] == '}') { out.append("{}", 2); ++j; }
                else out.append("{ ", 2);
                break;
            case '}': out.append(" }", 2); break;
            case ':': out.append(": ", 2); break;
            default:  out.append(", ", 2); break;
            }
            i = j + 1;
        }
    }

    // Emits the value at c[i]; returns the index just past it.
    std::size_t emitValue(std::size_t i, std::size_t indent, std::size_t column, bool root) {
        const char ch = c[i];
        if (ch != '{' && ch != '[') {
            std::size_t e = i;
            if (ch == '"') e = stringEnd(c, i) + 1;
            else while (e < c.size() && c[e] != ',' && c[e] != '}' && c[e] != ']') ++e;
            out.append(c.data() + i, e - i);
            return e;
        }
        const Container& k = table[ci];
        const std::size_t end = k.end;
        const bool trailingComma = end + 1 < c.size() && c[end + 1] == ',';
        const std::size_t oneLine = (end - i + 1) + (k.extraEnd - k.extraStart);
        if (end == i + 1 || (!root && column + oneLine + (trailingComma ? 1 : 0) <= width)) {
            emitOneLine(i, end);
            ci = k.next;
            return end + 1;
        }
        ++ci;
        const bool object = ch == '{';
        out.push_back(ch);
        std::size_t p = i + 1;
        const std::size_t inner = indent + 2;
        while (p < end) {
            out.push_back('\n');
            out.append(inner, ' ');
            std::size_t col = inner;
            if (object) {
                const std::size_t ke = stringEnd(c, p);
                out.append(c.data() + p, ke - p + 1);
                out.append(": ", 2);
                col += (ke - p + 1) + 2;
                p = ke + 2;   // past the ':'
            }
            p = emitValue(p, inner, col, false);
            if (c[p] == ',') { out.push_back(','); ++p; }
        }
        out.push_back('\n');
        out.append(indent, ' ');
        out.push_back(object ? '}' : ']');
        return end + 1;
    }
};

} // namespace

std::string prettyPrint(std::string_view compact, int width) {
    if (compact.empty()) return {};
    Layout L{compact, static_cast<std::size_t>(width), {}, 0, {}};
    L.table.reserve(compact.size() / 16);
    std::vector<uint32_t> stack;
    uint32_t extra = 0;
    const std::size_t n = compact.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = compact[i];
        switch (ch) {
        case '"':
            i = stringEnd(compact, i);
            break;
        case '{':
        case '[': {
            stack.push_back(static_cast<uint32_t>(L.table.size()));
            L.table.push_back(Container{static_cast<uint32_t>(i), 0, extra, 0, 0});
            if (ch == '{' && i + 1 < n && compact[i + 1] != '}') extra += 2;
            break;
        }
        case '}':
        case ']': {
            Container& k = L.table[stack.back()];
            stack.pop_back();
            k.end = static_cast<uint32_t>(i);
            k.extraEnd = extra;
            k.next = static_cast<uint32_t>(L.table.size());
            break;
        }
        case ':':
        case ',':
            ++extra;
            break;
        default:
            break;
        }
    }
    L.out.reserve(n + n / 2);
    L.emitValue(0, 0, 0, true);
    return std::move(L.out);
}

} // namespace ohao::reflect
