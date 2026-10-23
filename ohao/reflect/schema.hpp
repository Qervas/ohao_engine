#pragma once
// Machine-readable descriptions of a TypeRegistry for agents and tools in any language.
#include "reflect/registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ohao::reflect {

// Registration errors plus what agents need from every type: docs on all types, fields
// and enum values; a unit (or an explicit unitless()) on every numeric field of a
// serialized type. Empty means clean. Each entry starts with "Type.field:".
[[nodiscard]] std::vector<std::string> lint(const TypeRegistry& r);

// The catalogue (ohao_schema.json): every type and enum with fields, kinds, defaults,
// limits, units, docs and versions, in registration order. Canonical and deterministic;
// pretty layout plus a trailing newline. `schemaHash` (16 hex digits) is FNV-1a 64 of
// the catalogue's compact form without the schemaHash member.
[[nodiscard]] std::string exportCatalogue(const TypeRegistry& r);
[[nodiscard]] uint64_t schemaHash(const TypeRegistry& r);

// JSON Schema (draft 2020-12) for scene documents (*.scene.json) built from the serialized types:
// any validator can check a scene's shape, enums, limits and id syntax. Cross-entity
// rules (unique ids, parents, cycles, singletons) stay with the engine's validator.
[[nodiscard]] std::string exportSceneJsonSchema(const TypeRegistry& r);

} // namespace ohao::reflect
