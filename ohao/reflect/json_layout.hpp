#pragma once
// A JSON value built as a flat list of nodes, then laid out in one pass: pretty (exactly
// JsonWriter's JsonLayout::Pretty) or compact. Because every container's one-line length
// is known before anything is emitted, the pretty layout is decided up front and each
// byte is written once; JsonWriter instead writes one-line forms and re-lays out the
// containers that turn out too long.
//
// The scene save fills it with per-type writers (ecs/scene_writers.cpp). Canonical
// compact JSON (a generic writer's output, a preserved component) can be added as one
// value and gets the same layout.
//
// Keys are given as their JSON token, quoted and escaped ("\"position\""), and are
// referenced, not copied: they must outlive the emission (per-type writers pass string
// literals). compact() copies its key and text.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

class LayoutTree {
public:
    void clear();

    // A member of an object takes its key; an array element or a top-level value none.
    void beginObject(std::string_view key = {});
    void endObject();
    void beginArray(std::string_view key = {});
    void endArray();
    void f32(std::string_view key, float v);             // non-finite: fail("non-finite float"), null
    void floats(std::string_view key, const float* v, int n);   // an array of f32
    void i64(std::string_view key, int64_t v);
    void boolean(std::string_view key, bool v);
    void null(std::string_view key);
    void string(std::string_view key, std::string_view s);
    void hex64(std::string_view key, uint64_t v);        // "0123456789abcdef"
    void token(std::string_view key, std::string_view jsonToken);   // a scalar, verbatim
    // Canonical compact JSON (as JsonWriter writes it in Compact layout), as one value.
    void compact(std::string_view key, std::string_view json);

    // The first failure (a value that cannot be written canonically); the value is null.
    void fail(std::string message);
    [[nodiscard]] bool ok() const noexcept { return m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

    // The top-level values, in the order they were added (node indices).
    [[nodiscard]] const std::vector<uint32_t>& roots() const noexcept { return m_roots; }
    // A value's length on one line ("{ "a": 1 }", "[1, 2]", a scalar's text).
    [[nodiscard]] uint32_t oneLine(uint32_t node) const noexcept { return m_nodes[node].oneLine; }
    // An upper bound on the bytes any emission of every top-level value needs when they
    // are nested `baseDepth` containers deep.
    [[nodiscard]] std::size_t bound(uint32_t baseDepth) const noexcept {
        return m_bytes + m_nodes.size() * (2u * baseDepth + 2u);
    }

    // Pretty: writes node `i` at `out` as JsonWriter lays out a value whose first character
    // sits at `column`, nested `depth` containers deep (its children are indented
    // 2 * (depth + 1)), followed by a comma when `trailingComma`. The document's root
    // always breaks. Returns the end of what was written.
    char* emitPretty(char* out, uint32_t i, uint32_t column, uint32_t depth, bool trailingComma, bool root = false,
                     uint32_t width = 100) const;
    char* emitOneLine(char* out, uint32_t i) const;
    char* emitCompact(char* out, uint32_t i) const;
    // The same, appended to a string.
    void appendPretty(std::string& out, uint32_t i, uint32_t column, uint32_t depth, bool trailingComma,
                      bool root = false, uint32_t width = 100) const;
    void appendCompact(std::string& out, uint32_t i) const;

private:
    // FloatArray: an array of numbers held as one node; its text is the elements joined
    // by ',' (count of them). It lays out exactly like an Array of number leaves.
    enum class NodeKind : uint8_t { Leaf, Object, Array, FloatArray };
    struct Node {
        const char* key;    // the key token, or null when it lives in m_text (keyOff)
        uint32_t keyOff;
        uint32_t text;      // leaf: offset of its text in m_text
        uint32_t textLen;   // leaf: its length
        uint32_t end;       // container: index one past its last descendant
        uint32_t oneLine;   // see oneLine()
        uint32_t count;     // container: number of children
        uint16_t keyLen;    // 0: no key
        NodeKind kind;
    };

    uint32_t add(std::string_view key, NodeKind kind, bool keyInText = false);
    void leafDone(uint32_t node);
    void childDone(uint32_t child);
    void open(std::string_view key, bool object, bool keyInText = false);
    void close();
    std::size_t compactValue(std::size_t base, std::size_t i, std::size_t n, std::string_view key, bool keyInText);
    [[nodiscard]] const char* keyOf(const Node& n) const noexcept { return n.key ? n.key : m_text.data() + n.keyOff; }
    [[nodiscard]] uint32_t next(uint32_t i) const noexcept {
        return m_nodes[i].kind == NodeKind::Leaf || m_nodes[i].kind == NodeKind::FloatArray ? i + 1 : m_nodes[i].end;
    }
    char* emitFloatArray(char* out, const Node& n, bool oneLine, uint32_t depth) const;

    std::vector<Node> m_nodes;
    std::vector<uint32_t> m_open;
    std::vector<uint32_t> m_roots;
    std::string m_text;
    std::string m_error;
    std::size_t m_bytes{0};   // see bound()
};

} // namespace ohao::reflect
