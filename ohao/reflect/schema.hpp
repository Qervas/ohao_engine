#pragma once
// Machine-readable descriptions of a TypeRegistry for agents and tools in any language.
#include "reflect/registry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::reflect {

// Registration errors plus what agents need from every type: docs on all types, fields
// and enum values; a unit (or an explicit unitless()) on every numeric field of a
// serialized type. Empty means clean. Each entry starts with "Type.field:".
[[nodiscard]] std::vector<std::string> lint(const TypeRegistry& r);

// One entry of the API method table (owned by the layer that implements the methods).
struct MethodDoc {
    std::string_view name;
    const TypeInfo* params{nullptr};   // the params struct; its JSON Schema is the input schema
    std::string_view result;           // a compact description of the result
    std::string_view doc;
};

// JSON Schema (draft 2020-12) of a params struct, self-contained (no $ref) and terse:
// {"type": "object", "properties": {...}, "required"?: [...], "additionalProperties": false},
// each property with its description, type keywords, limits and (unless optional, free
// JSON or required) its default. The catalogue's method table and MCP's tool input
// schemas are this.
void writeParamsSchema(JsonWriter& w, const TypeInfo& params);

struct CatalogueOptions {
    std::span<const MethodDoc> methods{};
    JsonLayout layout{JsonLayout::Pretty};   // Pretty ends with '\n'; Compact has no newline
};

// The catalogue (ohao_schema.json): the error codes, the API methods, and every type and
// enum with fields, kinds, defaults, limits, units, docs and versions, in registration
// order. Canonical and deterministic. `schemaHash` (16 hex digits) is FNV-1a 64 of the
// catalogue's compact form without the schemaHash member.
[[nodiscard]] std::string exportCatalogue(const TypeRegistry& r, const CatalogueOptions& options = {});
[[nodiscard]] uint64_t schemaHash(const TypeRegistry& r, std::span<const MethodDoc> methods = {});

// JSON Schema (draft 2020-12) for scene documents (*.scene.json) built from the serialized types:
// any validator can check a scene's shape, enums, limits and id syntax. Cross-entity
// rules (unique ids, parents, cycles, singletons) stay with the engine's validator.
[[nodiscard]] std::string exportSceneJsonSchema(const TypeRegistry& r);

} // namespace ohao::reflect
