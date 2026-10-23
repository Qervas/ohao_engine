#pragma once
// A read-only JSON tree, the input of typed decoding and (step 4) of migrations. Built
// by a parser outside reflect (ecs/json_parse uses simdjson); reflect only reads it.
// Numbers keep their raw source token, so a float is parsed straight from its text and
// never through a double.
#include <cstdint>
#include <memory>
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

class JsonWriter;
// Writes a subtree canonically (strings re-escaped, numbers as their raw tokens, member
// order kept).
void writeJson(JsonWriter& w, const JsonDocument& doc, const JsonNode& n);

// JSON's number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
[[nodiscard]] bool isJsonNumber(std::string_view token, bool* isInteger = nullptr) noexcept;

} // namespace ohao::reflect
