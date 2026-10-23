#pragma once
// A read-only JSON tree, the input of typed decoding and (step 4) of migrations. Built
// by a parser outside reflect (ecs/json_parse uses simdjson); reflect only reads it.
// Numbers keep their raw source token, so a float is parsed straight from its text and
// never through a double.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

enum class JsonType : uint8_t { Null, Bool, Number, String, Array, Object };

[[nodiscard]] const char* jsonTypeName(JsonType t) noexcept;

struct JsonNode {
    JsonType type{JsonType::Null};
    bool boolean{false};
    uint32_t offset{0};      // byte offset of the value in JsonDocument::source
    uint32_t first{0};       // Array/Object: first child slot in JsonDocument::children
    uint32_t count{0};       // Array/Object: number of children
    std::string_view text;   // Number: raw token. String: the unescaped value
    std::string_view key;    // object members: the unescaped key
};

struct JsonDocument {
    std::vector<JsonNode> nodes;        // nodes[0] is the root
    std::vector<uint32_t> children;     // node indices, contiguous per container
    std::string_view source;            // the parsed text (offsets point into it)
    std::shared_ptr<void> storage;      // keeps `source`, keys and strings alive

    [[nodiscard]] const JsonNode& root() const { return nodes[0]; }
    [[nodiscard]] const JsonNode& child(const JsonNode& n, uint32_t i) const {
        return nodes[children[n.first + i]];
    }
    // The first member with this key, or null.
    [[nodiscard]] const JsonNode* member(const JsonNode& object, std::string_view k) const;
};

// A JSON value left as is inside a decoded struct (Kind::Json), for members that another
// decoder checks (a batch of commands, an inline scene). It points into the document
// being decoded and is valid as long as that document is. Default: absent.
struct JsonRef {
    const JsonDocument* doc{nullptr};
    const JsonNode* node{nullptr};
    [[nodiscard]] explicit operator bool() const noexcept { return node != nullptr; }
};

class JsonWriter;
// Writes a subtree canonically (strings re-escaped, numbers as their raw tokens, member
// order kept).
void writeJson(JsonWriter& w, const JsonDocument& doc, const JsonNode& n);

// JSON's number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
[[nodiscard]] bool isJsonNumber(std::string_view token, bool* isInteger = nullptr) noexcept;

// A small owning, mutable JSON value: what component upgrades (migrations) edit before
// the upgraded data is decoded. Numbers keep their token text, like JsonNode. Object
// member order is kept.
class JsonValue {
public:
    JsonValue() = default;   // null
    [[nodiscard]] static JsonValue boolean(bool b);
    [[nodiscard]] static JsonValue numberToken(std::string token);   // a valid JSON number
    [[nodiscard]] static JsonValue number(float v);                  // canonical shortest form
    [[nodiscard]] static JsonValue number(int64_t v);
    [[nodiscard]] static JsonValue string(std::string s);
    [[nodiscard]] static JsonValue array();
    [[nodiscard]] static JsonValue object();

    [[nodiscard]] JsonType type() const noexcept { return m_type; }
    [[nodiscard]] bool isNull() const noexcept { return m_type == JsonType::Null; }
    [[nodiscard]] bool asBool() const noexcept { return m_bool; }
    [[nodiscard]] std::string_view text() const noexcept { return m_text; }   // number token / string
    [[nodiscard]] std::optional<float> asFloat() const;     // numbers only, correctly rounded
    [[nodiscard]] std::optional<int64_t> asInt() const;     // integer numbers only

    // Array elements, or object member values (parallel to the keys).
    [[nodiscard]] std::vector<JsonValue>& items() noexcept { return m_items; }
    [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return m_items; }
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return m_keys; }

    // Objects. A missing key: find() returns null, erase()/rename() return false.
    [[nodiscard]] JsonValue* find(std::string_view key);
    [[nodiscard]] const JsonValue* find(std::string_view key) const;
    JsonValue& set(std::string_view key, JsonValue v);   // replaces in place, or appends
    bool erase(std::string_view key);
    bool rename(std::string_view from, std::string_view to);   // keeps the position; false if `to` exists

private:
    JsonType m_type{JsonType::Null};
    bool m_bool{false};
    std::string m_text;
    std::vector<JsonValue> m_items;
    std::vector<std::string> m_keys;
};

[[nodiscard]] JsonValue toValue(const JsonDocument& doc, const JsonNode& n);
void writeJson(JsonWriter& w, const JsonValue& v);
// A read-only document over a value (its nodes point into the document's own copy), so
// upgraded data goes through the same typed decoder as everything else. Offsets are 0.
[[nodiscard]] JsonDocument toDocument(JsonValue v);

} // namespace ohao::reflect
