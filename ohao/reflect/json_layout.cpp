#include "reflect/json_layout.hpp"

#include "reflect/json_writer.hpp"

#include <charconv>
#include <cmath>
#include <cstring>

namespace ohao::reflect {

namespace {

// Index of the closing quote of the string token that starts at s[i] == '"'.
std::size_t stringEnd(const char* s, std::size_t i, std::size_t n) {
    ++i;
    while (i < n) {
        const char c = s[i];
        if (c == '"') return i;
        i += (c == '\\') ? 2 : 1;
    }
    return n - 1;
}

inline char* put(char* o, const char* s, std::size_t n) {
    std::memcpy(o, s, n);
    return o + n;
}

} // namespace

void LayoutTree::clear() {
    m_nodes.clear();
    m_open.clear();
    m_roots.clear();
    m_text.clear();
    m_error.clear();
    m_bytes = 0;
}

uint32_t LayoutTree::add(std::string_view key, NodeKind kind, bool keyInText) {
    const auto idx = static_cast<uint32_t>(m_nodes.size());
    Node& n = m_nodes.emplace_back();
    n.key = keyInText ? nullptr : key.data();
    n.keyOff = keyInText ? static_cast<uint32_t>(key.data() - m_text.data()) : 0;
    n.keyLen = static_cast<uint16_t>(key.size());
    n.text = 0;
    n.textLen = 0;
    n.end = idx + 1;
    n.oneLine = 0;
    n.count = 0;
    n.kind = kind;
    // Key, ": ", ", ", brackets with their padding, and a line's newline and indent.
    m_bytes += key.size() + 12u + 2u * m_open.size();
    if (m_open.empty()) m_roots.push_back(idx);
    return idx;
}

// A finished child adds itself to its parent's one-line length: ", " between siblings
// and "{ " / " }" or "[" / "]" are added when the parent closes.
void LayoutTree::childDone(uint32_t child) {
    if (m_open.empty()) return;
    Node& p = m_nodes[m_open.back()];
    const Node& c = m_nodes[child];
    p.oneLine += c.oneLine + (p.kind == NodeKind::Object ? c.keyLen + 2u : 0u);
    ++p.count;
}

void LayoutTree::leafDone(uint32_t node) {
    Node& n = m_nodes[node];
    n.oneLine = n.textLen;
    m_bytes += n.textLen;
    childDone(node);
}

void LayoutTree::open(std::string_view key, bool object, bool keyInText) {
    m_open.push_back(add(key, object ? NodeKind::Object : NodeKind::Array, keyInText));
}

void LayoutTree::close() {
    const uint32_t idx = m_open.back();
    m_open.pop_back();
    Node& n = m_nodes[idx];
    n.end = static_cast<uint32_t>(m_nodes.size());
    if (n.count == 0) n.oneLine = 2;   // "{}" / "[]"
    else n.oneLine += (n.kind == NodeKind::Object ? 4u : 2u) + 2u * (n.count - 1);
    childDone(idx);
}

void LayoutTree::beginObject(std::string_view key) { open(key, true); }
void LayoutTree::endObject() { close(); }
void LayoutTree::beginArray(std::string_view key) { open(key, false); }
void LayoutTree::endArray() { close(); }

void LayoutTree::token(std::string_view key, std::string_view jsonToken) {
    const uint32_t n = add(key, NodeKind::Leaf);
    m_nodes[n].text = static_cast<uint32_t>(m_text.size());
    m_nodes[n].textLen = static_cast<uint32_t>(jsonToken.size());
    m_text.append(jsonToken);
    leafDone(n);
}

void LayoutTree::f32(std::string_view key, float v) {
    if (!std::isfinite(v)) {
        fail("non-finite float");
        null(key);
        return;
    }
    char buf[32];
    token(key, std::string_view(buf, static_cast<std::size_t>(formatFloat32(v, buf) - buf)));
}

void LayoutTree::floats(std::string_view key, const float* v, int n) {
    if (n == 0) {
        beginArray(key);
        endArray();
        return;
    }
    const uint32_t node = add(key, NodeKind::FloatArray);
    const std::size_t start = m_text.size();
    m_text.resize(start + static_cast<std::size_t>(n) * 33);
    char* const base = m_text.data();
    char* o = base + start;
    for (int i = 0; i < n; ++i) {
        if (i) *o++ = ',';
        if (std::isfinite(v[i])) {
            o = formatFloat32(v[i], o);
        } else {
            fail("non-finite float");
            o = put(o, "null", 4);
        }
    }
    m_text.resize(static_cast<std::size_t>(o - base));
    Node& nd = m_nodes[node];
    nd.text = static_cast<uint32_t>(start);
    nd.textLen = static_cast<uint32_t>(m_text.size() - start);
    nd.count = static_cast<uint32_t>(n);
    nd.oneLine = nd.textLen + 2u + static_cast<uint32_t>(n - 1);   // "[" and "]", a space after each ','
    m_bytes += nd.textLen + static_cast<std::size_t>(n) * (2u * m_open.size() + 8u);
    childDone(node);
}

// An array of numbers: compact, one line, or one element per line (indented for `depth`).
char* LayoutTree::emitFloatArray(char* o, const Node& n, bool oneLine, uint32_t depth) const {
    const char* s = m_text.data() + n.text;
    const char* const e = s + n.textLen;
    if (!oneLine) {
        const uint32_t indent = 2u * (depth + 1u);
        *o++ = '[';
        for (;;) {
            *o++ = '\n';
            std::memset(o, ' ', indent);
            o += indent;
            const char* c = static_cast<const char*>(std::memchr(s, ',', static_cast<std::size_t>(e - s)));
            const char* stop = c ? c + 1 : e;   // the element and its comma
            o = put(o, s, static_cast<std::size_t>(stop - s));
            if (!c) break;
            s = stop;
        }
        *o++ = '\n';
        std::memset(o, ' ', indent - 2u);
        o += indent - 2u;
        *o++ = ']';
        return o;
    }
    *o++ = '[';
    for (; s < e; ++s) {
        *o++ = *s;
        if (*s == ',') *o++ = ' ';
    }
    *o++ = ']';
    return o;
}

void LayoutTree::i64(std::string_view key, int64_t v) {
    char buf[24];
    token(key, std::string_view(buf, static_cast<std::size_t>(std::to_chars(buf, buf + sizeof(buf), v).ptr - buf)));
}

void LayoutTree::boolean(std::string_view key, bool v) { token(key, v ? "true" : "false"); }
void LayoutTree::null(std::string_view key) { token(key, "null"); }

void LayoutTree::string(std::string_view key, std::string_view s) {
    const uint32_t n = add(key, NodeKind::Leaf);
    m_nodes[n].text = static_cast<uint32_t>(m_text.size());
    appendJsonString(m_text, s);
    m_nodes[n].textLen = static_cast<uint32_t>(m_text.size()) - m_nodes[n].text;
    leafDone(n);
}

void LayoutTree::hex64(std::string_view key, uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    char buf[18];
    buf[0] = '"';
    for (int i = 0; i < 16; ++i) buf[1 + i] = kDigits[(v >> (60 - 4 * i)) & 0xF];
    buf[17] = '"';
    token(key, std::string_view(buf, 18));
}

void LayoutTree::compact(std::string_view key, std::string_view json) {
    if (json.empty()) {
        null(key);
        return;
    }
    // The key and text are copied once; the keys and scalars inside are referenced where
    // they lie.
    const std::size_t keyBase = m_text.size();
    m_text.append(key);
    const std::size_t base = m_text.size();
    m_text.append(json);
    const std::string_view copied(m_text.data() + keyBase, key.size());
    compactValue(base, 0, json.size(), copied, !key.empty());
}

// Adds the canonical compact value at m_text[base + i] (the text is base .. base + n);
// returns the index just past it. Nothing is appended to m_text meanwhile.
std::size_t LayoutTree::compactValue(std::size_t base, std::size_t i, std::size_t n, std::string_view key,
                                     bool keyInText) {
    const char* s = m_text.data() + base;
    const char c = s[i];
    if (c == '{' || c == '[') {
        const bool object = c == '{';
        open(key, object, keyInText);
        ++i;
        while (i < n && s[i] != (object ? '}' : ']')) {
            std::string_view k;
            if (object) {
                const std::size_t e = stringEnd(s, i, n);
                k = std::string_view(s + i, e - i + 1);
                i = e + 2;   // past the ':'
            }
            i = compactValue(base, i, n, k, object);
            if (i < n && s[i] == ',') ++i;
        }
        close();
        return i + 1;
    }
    std::size_t e = i;
    if (c == '"') e = stringEnd(s, i, n) + 1;
    else while (e < n && s[e] != ',' && s[e] != '}' && s[e] != ']') ++e;
    const uint32_t node = add(key, NodeKind::Leaf, keyInText);
    m_nodes[node].text = static_cast<uint32_t>(base + i);
    m_nodes[node].textLen = static_cast<uint32_t>(e - i);
    leafDone(node);
    return e;
}

void LayoutTree::fail(std::string message) {
    if (m_error.empty()) m_error = std::move(message);
}

// ---- emission ----------------------------------------------------------------------------

char* LayoutTree::emitOneLine(char* o, uint32_t i) const {
    const Node& n = m_nodes[i];
    if (n.kind == NodeKind::Leaf) return put(o, m_text.data() + n.text, n.textLen);
    if (n.kind == NodeKind::FloatArray) return emitFloatArray(o, n, true, 0);
    const bool object = n.kind == NodeKind::Object;
    if (n.count == 0) return put(o, object ? "{}" : "[]", 2);
    if (object) o = put(o, "{ ", 2);
    else *o++ = '[';
    bool first = true;
    for (uint32_t j = i + 1; j < n.end; j = next(j)) {
        if (!first) o = put(o, ", ", 2);
        first = false;
        if (object) {
            const Node& c = m_nodes[j];
            o = put(o, keyOf(c), c.keyLen);
            o = put(o, ": ", 2);
        }
        o = emitOneLine(o, j);
    }
    if (object) return put(o, " }", 2);
    *o++ = ']';
    return o;
}

char* LayoutTree::emitPretty(char* o, uint32_t i, uint32_t column, uint32_t depth, bool trailingComma, bool root,
                             uint32_t width) const {
    const Node& n = m_nodes[i];
    // JsonWriter's rule: a container stays on one line when it is empty, or when it is not
    // the root and its one-line form (plus a trailing comma) ends within `width` columns.
    // A child of a broken container is judged at its own line's position; a container
    // that fits never holds a child that does not (the child would sit further left).
    if (n.kind == NodeKind::Leaf || n.count == 0 || (!root && column + n.oneLine + (trailingComma ? 1u : 0u) <= width))
        return emitOneLine(o, i);
    if (n.kind == NodeKind::FloatArray) return emitFloatArray(o, n, false, depth);
    const bool object = n.kind == NodeKind::Object;
    const uint32_t indent = 2u * (depth + 1u);
    *o++ = object ? '{' : '[';
    uint32_t k = 0;
    for (uint32_t j = i + 1; j < n.end; j = next(j), ++k) {
        *o++ = '\n';
        std::memset(o, ' ', indent);
        o += indent;
        uint32_t col = indent;
        if (object) {
            const Node& c = m_nodes[j];
            o = put(o, keyOf(c), c.keyLen);
            o = put(o, ": ", 2);
            col += c.keyLen + 2u;
        }
        const bool last = k + 1 == n.count;
        o = emitPretty(o, j, col, depth + 1, !last, false, width);
        if (!last) *o++ = ',';
    }
    *o++ = '\n';
    std::memset(o, ' ', indent - 2u);
    o += indent - 2u;
    *o++ = object ? '}' : ']';
    return o;
}

char* LayoutTree::emitCompact(char* o, uint32_t i) const {
    const Node& n = m_nodes[i];
    if (n.kind == NodeKind::Leaf) return put(o, m_text.data() + n.text, n.textLen);
    if (n.kind == NodeKind::FloatArray) {
        *o++ = '[';
        o = put(o, m_text.data() + n.text, n.textLen);
        *o++ = ']';
        return o;
    }
    const bool object = n.kind == NodeKind::Object;
    *o++ = object ? '{' : '[';
    bool first = true;
    for (uint32_t j = i + 1; j < n.end; j = next(j)) {
        if (!first) *o++ = ',';
        first = false;
        if (object) {
            const Node& c = m_nodes[j];
            o = put(o, keyOf(c), c.keyLen);
            *o++ = ':';
        }
        o = emitCompact(o, j);
    }
    *o++ = object ? '}' : ']';
    return o;
}

void LayoutTree::appendPretty(std::string& out, uint32_t i, uint32_t column, uint32_t depth, bool trailingComma,
                              bool root, uint32_t width) const {
    const std::size_t old = out.size();
    out.resize(old + bound(depth));
    char* end = emitPretty(out.data() + old, i, column, depth, trailingComma, root, width);
    out.resize(static_cast<std::size_t>(end - out.data()));
}

void LayoutTree::appendCompact(std::string& out, uint32_t i) const {
    const std::size_t old = out.size();
    out.resize(old + bound(0));
    char* end = emitCompact(out.data() + old, i);
    out.resize(static_cast<std::size_t>(end - out.data()));
}

} // namespace ohao::reflect
