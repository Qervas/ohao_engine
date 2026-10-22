# Sub-project 3, build step 1: ohao_reflect

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 1)
**Status:** Done; committed in `4a1268f` (local, not pushed).

## 1. Summary

Step 1 adds `ohao_reflect`, a static library in `ohao/reflect` that depends on glm and the standard library only. It holds the type registry, the typed value codec, the canonical JSON writer and the schema export (the agent catalogue and a JSON Schema, draft 2020-12). It also adds the tests in `tests/reflect` and `external/cmake/pfr.cmake`, which fetches Boost.PFR for the tests only.

Nothing else in the engine changed. The only other edits are one `add_subdirectory(reflect)` line in `ohao/CMakeLists.txt` and a `BUILD_REFLECT_TESTS` option (default ON) in the root `CMakeLists.txt` that adds `tests/reflect`. No engine target links `ohao_reflect` yet.

## 2. Library contents

### 2.1 `json_writer.hpp` / `.cpp`

`JsonWriter` writes canonical compact JSON: no whitespace, and the same calls always produce the same bytes.

- Floats use `std::to_chars` in its shortest round-trip form. A fast path writes whole numbers with |v| < 10^4 as integers. `writer_fast_path_matches_to_chars` checks that the output is identical to `to_chars`. `-0` is written as `-0`.
- Every string goes through one escaper. `"` and `\` are escaped, and control characters are escaped (`\b \f \n \r \t`, otherwise `\u00XX`).
- `uint64()` writes decimal. `hex64()` writes 16 lowercase hex digits in quotes. The codec uses `hex64` for `UInt64` fields and entity ids.
- A non-finite float writes `null` and records an error. An enum value with no registered name also writes `null` and records an error. `ok()` and `error()` report the first error.
- `raw()` copies pre-canonical JSON verbatim.
- `prettyPrint(compact, width = 100)` uses 2-space indentation. A container prints on one line if it fits in `width` columns, counting its key and trailing comma. The root container always breaks. One-line objects pad their braces (`{ "a": 1 }`); arrays do not. Removing whitespace outside strings gives back the compact input.
- `fnv1a64` is a constexpr 64-bit FNV-1a. `schemaHash` uses it.
- `EntityIdMapper` is an interface that maps a raw entity value to a stable 64-bit id. `JsonWriter::entityIds` points at it. When it is null, every entity reference writes `null`.

### 2.2 `asset_ref.hpp`

`AssetRef { path, guid }`. `path` is project-relative with `/` separators. `guid` stays empty until the asset database exists, and scene files write it only when it is set. The type has a defaulted `operator==`.

### 2.3 `type_info.hpp`

- `Kind` has 17 values: Bool, Int32, UInt32, UInt64, Float, String, Vec2, Vec3, Vec4, Quat, Enum, Struct, Vector, Optional, Variant, AssetRef, EntityRef.
- `TypeDesc` is a kind plus what it holds: `structType`, `enumType`, `element` (vector and optional) and `alternatives` (variant, in `std::variant` order). The registry interns these, and their pointers stay valid.
- `FieldAttrs` holds doc, unit, unitless, `NumericLimits` (min, max), uiRange, step, tooltip, color, angle, hidden, readOnly and aliases.
- `FieldOps` is the typed operation set for one member: `write`, `equal`, `inRange`, and `address` (mutable and const). Each call is one virtual dispatch into code that knows the exact C++ type.
- `FieldInfo` pairs a name, a `TypeDesc`, `FieldAttrs` and a `FieldOps`. `SkippedField` holds a name and a reason.
- `TypeRole` is `Component` (a top-level block in scene files) or `Struct` (a nested value).
- `TypeInfo` holds name, doc, version, role, `serialized`, `singleton`, the fields in registration order, the skipped members, size, alignment, a trivially-copyable flag, construct, destroy and copy function pointers, and a shared default-constructed instance. `field(name)` looks a field up.
- `EnumValue` and `EnumInfo` hold the values in registration order, with `findByValue` and `findByName`.
- `writeStruct` and `equalStruct` walk the registered fields and ignore skipped members. `typeName` returns the catalogue form (`optional<AssetRef>`, `variant<A|B>`). `isNumeric` and `takesUnit` classify kinds.

### 2.4 `codec.hpp`

Header-only function templates, instantiated for each C++ type.

- `kindOf<V>()` maps a C++ type to its `Kind`. An unsupported type fails with a `static_assert`.
- `writeValue` writes one value in canonical form. Vectors and optionals are arrays (optional `null`). A variant is an object with one key, the alternative's name. Quaternions are written in glTF order (x, y, z, w).
- `equalValue` is bit-exact for floats, so -0 and +0 differ.
- `inRangeValue` checks limits per component for vectors and per element for vector and optional fields.
- `MemberOps<C, M>` is the `FieldOps` for a member `M C::*`. Registration creates one for each field.
- `ValueTraits<V>` is the extension point for handle types. A specialization with `isEntityRef = true` and `toRaw` makes the type an `EntityRef` field. The ECS provides the `entt::entity` specialization in step 2. The comment also declares `fromRaw` and `null` for reading, which step 1 code does not use. Reflect never includes EnTT.

### 2.5 `registry.hpp` / `.cpp`

`TypeRegistry` is an explicit object filled by registration functions. It has no global state and no static registration.

- `enumeration<E>(name)` returns an `EnumBuilder` with `doc` and `value(name, E, doc)`.
- `type<T>(name, version = 1)` returns a `TypeBuilder`. Its calls are `doc`, `component`, `runtime`, `singleton`, `field`, `skip`, `unit`, `unitless`, `min`, `max`, `uiRange`, `step`, `tooltip`, `color`, `angle`, `hidden`, `readOnly` and `alias`. Field attributes apply to the most recent field. Before any field exists, `doc` sets the type's doc and the others are errors.
- `skip` takes the form `.skip("name", &T::m, "reason")`. The reason must not be empty.
- Lookup is by name (`find`, `findEnum`) or by C++ type (`find<T>()`, `findEnum<E>()`). `types()` and `enums()` return registration order.
- Nested structs and enums must be registered before the types that use them. A field whose type is not yet registered is an error.
- Mistakes are recorded in `errors()` rather than thrown. The recorded cases include duplicate or non-identifier names, an unknown unit, a unit on a non-numeric field, `min` greater than `max`, a default outside its limits, a skip with no reason, and `version` 0.
- Known units (`knownUnits()`, 17 in all): `m`, `m/s`, `m/s2`, `rad`, `deg`, `s`, `kg`, `N`, `px`, `linear-rgb`, `srgb`, `lux`, `cd`, `lm`, `nits`, `ratio`, `Hz`. `linear-rgb` is a color in linear light.

### 2.6 `schema.hpp` / `.cpp`

- `lint(r)` returns the registration errors plus these checks: missing doc on a type, field, enum or enum value; an enum with no values; a serialized component with no fields; a numeric field of a serialized type with neither `.unit()` nor `.unitless()`. An empty result means clean.
- `exportCatalogue(r)` writes `ohao_schema.json`. It has `format` (`ohao.schema`), `version` 1, `units`, `enums` and `types`. Every type is listed, saved or runtime, with its `serialized` flag. Each field gives its type, default, doc, unit, min, max, uiRange, step, tooltip, the color, angle, hidden and readOnly flags, aliases, and the skipped members with their reasons. A unitless field shows unit `"1"`. A field with neither a unit nor unitless shows `null`, and lint reports it. The output is pretty-printed with a trailing newline.
- `schemaHash(r)` is the FNV-1a 64 of the compact catalogue without the `schemaHash` member. `exportCatalogue` stores it as 16 hex digits.
- `exportSceneJsonSchema(r)` writes a JSON Schema (draft 2020-12) for `.ohscene` documents, with `$id` `urn:ohao:ohscene:1`. The top level requires `format` (const `ohao.scene`), `version` (const 1) and `entities`. A top-level `components` object has integer values of at least 1. Each entity requires an `id`, an `EntityId` (16 lowercase hex digits, not all zero). Known serialized components reference their `$defs`, and known structs reject unknown fields. Unknown component blocks are accepted as opaque objects. Numeric limits become `minimum` and `maximum`. `UInt64` is a 16-digit hex string. Enums are name strings, and variants are one-key objects. Cross-entity rules stay with the engine's validator.

## 3. Rules this step enforces

- **Reflection is never on a hot path.** `t13_no_reflection_on_hot_paths` scans `ohao/render`, `ohao/gpu` and the ECS transform and render-extract sources for `#include` lines naming `reflect/` or `api/`. It fails on any match.
- **`ohao_reflect` depends on glm and the standard library only.** `t13_reflect_depends_on_glm_and_std_only` fails if any file in `ohao/reflect` includes EnTT, Vulkan, a JSON library, or `core/`, `gpu/`, `render/` or `ecs/` headers. The CMake target links glm and nothing else.
- **Docs and units are required.** `lint()` enforces them, and `t01_lint_reports_missing_docs_and_units` checks it.
- **Completeness.** `t01_every_member_registered_or_skipped` uses Boost.PFR, test-only, to count a struct's members. Each member must be registered or skipped. `t01_completeness_catches_a_missing_member` checks that the count catches a gap.

## 4. Tests

`tests/reflect/reflect_tests.cpp` has 22 tests, and all pass. The test names carry the spec's numbers.

- **t01 (5):** completeness, lint of the fixture, lint of missing docs and units, and reporting of registration mistakes.
- **Writer and codec (10):** shortest round-trip floats, bit-exact float text, rejection of non-finite values, escaping and structure, the fast path, the pretty layout rules, stripping pretty output back to compact, canonical struct writes, bit-exact struct equality, and per-component limits.
- **t12 (3):** deterministic catalogue with its own hash, catalogue matches its golden, scene JSON Schema matches its golden.
- **t13 (2):** the hot-path rule and the dependency rule, both described in section 3.
- **Benchmarks (2):** informational, not gated. Section 5 has the numbers.

Golden files live in `tests/reflect/golden/` (`fixture_catalogue.json` and `fixture_ohscene.schema.json`). The t12 tests compare the exported text with them byte for byte. Regenerate them only after a deliberate format change. `.gitattributes` pins `tests/reflect/golden/**` and `*.ohscene` to LF, so `core.autocrlf=true` cannot turn a fresh checkout's goldens into CRLF and break the comparison.

`tests/reflect/check_scene_schema.py` dumps the fixture schema with `reflect_tests --dump-schema`. It checks that the schema is valid Draft 2020-12, then validates the samples. The 2 valid samples (`samples/valid/full.json`, `minimal.json`) are accepted, and the 12 invalid samples are rejected. Result: 14 of 14. The script needs the `jsonschema` package.

Build and run, from the repository root (Visual Studio generator, binaries in `build/Release/`):

```
/c/Users/djmax/scoop/shims/cmake --build build --config Release --target reflect_tests
build/Release/reflect_tests.exe
python tests/reflect/check_scene_schema.py build/Release/reflect_tests.exe
build/Release/reflect_tests.exe --update-golden
```

`reflect_tests.exe` prints a JSON summary as its last line and exits 0 on success. `--update-golden` rewrites the two golden files from the fixture registry.

## 5. Measured performance

Release build on Frank's laptop. These figures are informational and are not gated in step 1.

- 10k structs with 18 fields each: typed write 7.5 ms (3.9 MB compact). Pretty print 8.5 ms (5.4 MB). Best of 10 runs.
- 1M random floats to shortest text: 29 ns each. MSVC `to_chars` is the floor for this path.
- The pretty printer went from 19 ms to 8.5 ms after it switched from per-byte arrays to a per-container table.
- The step 6 budget (saving 10k entities in 10 ms or less) is still to be measured, on real components. Float formatting is the main cost to watch.

## 6. Spec adjustments made in this step

The spec was updated where step 1 differs from it or narrows it.

- simdjson and unordered_dense are fetched by the steps that first use them (step 3 and step 2), not in step 1. Step 1 fetches only Boost.PFR, pinned and test-only.
- The skip signature is `.skip("name", &T::m, "reason")`, and the reason is required.
- `.component()` and `.runtime()` are documented.
- A unitless field appears with unit `"1"` in the catalogue.
- The method table and the error table join the catalogue in steps 3 and 5. Step 1 has neither.

## 7. Deferred to later steps

- Decoding and reading, the two-phase load and structured validation: step 3.
- Registrations for the current authoring components, `EntityId`, and the `entt::entity` `ValueTraits` specialization: step 2.
- Versioning, migrations and `upgrade(from, fn)`: step 4. Step 1 has the `version` field but no upgrade hook.
- Wiring the catalogue and the scene schema into `ohao schema`: step 9.
