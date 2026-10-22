#include "reflect/json_writer.hpp"

#include <array>
#include <charconv>
#include <cmath>

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

} // namespace

// A comma is due exactly when the previous token was a value or a closed container.
void JsonWriter::separator() {
    if (m_needComma) m_out.push_back(',');
}

void JsonWriter::beginObject() { separator(); m_out.push_back('{'); m_needComma = false; }
void JsonWriter::endObject()   { m_out.push_back('}'); m_needComma = true; }
void JsonWriter::beginArray()  { separator(); m_out.push_back('['); m_needComma = false; }
void JsonWriter::endArray()    { m_out.push_back(']'); m_needComma = true; }

void JsonWriter::key(std::string_view k) {
    separator();
    appendString(k);
    m_out.push_back(':');
    m_needComma = false;
}

void JsonWriter::null() { separator(); m_out.append("null", 4); m_needComma = true; }
void JsonWriter::boolean(bool v) {
    separator();
    if (v) m_out.append("true", 4);
    else m_out.append("false", 5);
    m_needComma = true;
}

void JsonWriter::int64(int64_t v) {
    separator();
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, r.ptr);
    m_needComma = true;
}

void JsonWriter::uint64(uint64_t v) {
    separator();
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
    separator();
    char buf[32];
    // Fast path for whole numbers below 10^4, common in scenes (0, 1, scales, unit
    // quaternions). It prints exactly what to_chars does: shortest form picks fixed
    // notation whenever it is not longer than scientific, and for |v| < 10^4 fixed has
    // at most 4 digits while scientific needs at least 5 characters ("1e+03").
    if (std::fabs(v) < 10000.0f && v == static_cast<float>(static_cast<int32_t>(v))) {
        const auto i = static_cast<int32_t>(v);
        if (i == 0 && std::signbit(v)) {
            m_out.append("-0", 2);
        } else {
            auto r = std::to_chars(buf, buf + sizeof(buf), i);
            m_out.append(buf, r.ptr);
        }
        m_needComma = true;
        return;
    }
    auto r = std::to_chars(buf, buf + sizeof(buf), v);   // shortest round-trip
    m_out.append(buf, r.ptr);
    m_needComma = true;
}

void JsonWriter::hex64(uint64_t v) {
    separator();
    static constexpr char kDigits[] = "0123456789abcdef";
    char buf[18];
    buf[0] = '"';
    for (int i = 0; i < 16; ++i) buf[1 + i] = kDigits[(v >> (60 - 4 * i)) & 0xF];
    buf[17] = '"';
    m_out.append(buf, 18);
    m_needComma = true;
}

void JsonWriter::string(std::string_view s) {
    separator();
    appendString(s);
    m_needComma = true;
}

void JsonWriter::appendString(std::string_view s) {
    m_out.push_back('"');
    const char* p = s.data();
    const char* const end = p + s.size();
    while (p < end) {
        const char* run = p;
        while (p < end && !kNeedsEscape[static_cast<unsigned char>(*p)]) ++p;
        m_out.append(run, p);
        if (p == end) break;
        const unsigned char c = static_cast<unsigned char>(*p++);
        switch (c) {
        case '"':  m_out.append("\\\"", 2); break;
        case '\\': m_out.append("\\\\", 2); break;
        case '\b': m_out.append("\\b", 2); break;
        case '\f': m_out.append("\\f", 2); break;
        case '\n': m_out.append("\\n", 2); break;
        case '\r': m_out.append("\\r", 2); break;
        case '\t': m_out.append("\\t", 2); break;
        default: {
            static constexpr char kDigits[] = "0123456789abcdef";
            const char esc[6] = {'\\', 'u', '0', '0', kDigits[c >> 4], kDigits[c & 0xF]};
            m_out.append(esc, 6);
        }
        }
    }
    m_out.push_back('"');
}

void JsonWriter::raw(std::string_view compactJson) {
    separator();
    m_out.append(compactJson);
    m_needComma = true;
}

void JsonWriter::fail(std::string message) {
    if (m_error.empty()) m_error = std::move(message);
}

// ---------------------------------------------------------------------------------------
// Pretty printing

namespace {

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
