#pragma once
#include "core/result.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"

#include <memory>
#include <string_view>

namespace ohao::ecs {

// Parses UTF-8 JSON (strict RFC 8259) into a reflect::JsonDocument with a single-pass
// reader; simdjson validates the UTF-8. The document owns a padded copy of the text;
// strings, keys and number tokens point into it (escaped strings into an arena it also
// owns). On failure the diagnostic is E_PARSE at the failing byte, with line and column.
[[nodiscard]] Result<reflect::JsonDocument, reflect::Diagnostic> parseJson(std::string_view text);

// The UTF-8 check parseJson makes first (simdjson's SIMD validator).
[[nodiscard]] bool validUtf8(std::string_view text) noexcept;

// A reusable parser: keeps its padded input buffer, string arena and tree storage
// between parses, so repeated loads (an agent loading thousands of times) allocate
// nothing once warm. The returned document lives until the next parse() on this parser.
class JsonParser {
public:
    JsonParser();
    ~JsonParser();
    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;

    [[nodiscard]] Result<const reflect::JsonDocument*, reflect::Diagnostic> parse(std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ohao::ecs
