# Sub-project 3, build step 3: the JSON reader, two-phase load and validation

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 3)
**Status:** Done; committed in `328b0f0` (local, not pushed).

## 1. Summary

Step 3 reads and writes scene files in `ohao/ecs/scene_io`: `saveScene`, `validateScene`, `loadScene` and the file forms. Reading parses with simdjson into a read-only tree (`reflect::JsonDocument`), decodes the tree into typed values in `ohao_reflect`, and loads in two phases. The writer gains a single-pass pretty layout, and floats are parsed with fast_float. The error-code table is part of the catalogue. The error corpus `tests/scenes/invalid` is added.

## 2. The writer (`ohao/reflect/json_writer.*`)

### 2.1 Layouts

- **Compact** has no whitespace. It is used for the wire, queries and hashing, and `raw()` takes this form.
- **Pretty** uses a 2-space indent. A container prints on one line when it fits in `width` columns (default 100), counting its key and trailing comma. The root always breaks. One-line objects pad their braces (`{ "a": 1 }`); arrays do not. Empty containers print as `{}` and `[]`.

### 2.2 Single-pass pretty layout

No second scan is made. Each container is written in its one-line form. When it closes, it becomes the pending container. Its fit decision waits for the next token, because a trailing comma adds a column. The pending container stays at the tail of the buffer until then. If it does not fit, its bytes are rewritten in place, one child per line. The children are already in final form and carry absolute indents. A broken child forces its parent to break. A broken container's bytes are copied once per broken ancestor.

### 2.3 Equality with prettyPrint

`prettyPrint(compact, width)` is a separate implementation over finished text. Tools use it, and the tests use it as the reference. `pretty_single_pass_matches_pretty_print` writes the same generated document in both forms for seeds 1 to 20 and widths 16, 40, 80, 100 and 160. That is 100 cases, and each pair must match. `t02` checks the scene form: the pretty file equals `prettyPrint(compact) + "\n"`.

### 2.4 Numbers, raw and numberToken

- `float32` writes the shortest round-trip form (`std::to_chars`), with a fast path for whole numbers below 10^4. `-0` keeps its sign. A non-finite value writes `null` and records an error, which `ok()` reports.
- `numberToken(token)` writes a valid JSON number verbatim. `writeJson` uses it for tree numbers, so they round-trip exactly.
- `hex64(v)` writes 16 lowercase hex digits in quotes. Entity ids use it.
- `raw(compactJson)` inserts a compact JSON value, such as a preserved component. Compact appends the bytes. Pretty re-emits the value through the writer, so it gets the same layout as everything else. Empty input writes `null`.

## 3. The JSON tree and decoding (`ohao/reflect`)

### 3.1 JsonDocument and JsonNode

- `JsonNode` is 48 bytes: `type`, `boolean`, the byte `offset`, `first` and `count` (indices into `children`), `text` and `key`. For a number, `text` is the raw token. For a string, it is the unescaped value.
- `JsonDocument` holds `nodes` (`nodes[0]` is the root), `children`, `source`, and `storage`, which keeps the source, keys and strings alive. `member()` returns the first member with a given key.
- Numbers are never converted to double. `isJsonNumber` checks JSON's grammar and reports whether a token is an integer.

### 3.2 DecodeContext and Diagnostic

- `DecodeContext` holds the document, `errors`, `warnings`, the pending entity references, the current entity id and component, and a path stack. `path()` is a JSON Pointer with RFC 6901 escaping, for example `/entities/0/Light/intensity`.
- At most 100 errors are kept. The 101st appends one `E_TOO_MANY_ERRORS` and sets `full()`, and decoders stop early. Warnings are not capped.
- `Diagnostic` has `code`, `path`, `entity`, `offset`, `line`, `column`, `message`, `expected`, `got` and `suggestion`. `writeDiagnostic` always writes `code` and `path`, and writes the other members only when they carry something. `fillLineColumns` converts offsets to 1-based line and column. Errors are sorted by offset with a stable sort, and `E_TOO_MANY_ERRORS` stays last.

### 3.3 suggest()

`suggest(word, candidates)` uses the optimal string alignment distance, compared case-insensitively. Insertion, deletion, substitution and a swap of two adjacent letters each cost 1, so `Spto` is 1 from `Spot`. The threshold is `max(1, |word| / 3)`. It is used for unknown fields, root members, enum values, variant alternatives and components.

### 3.4 Scalars

- `readFloat` parses the token with `fast_float::from_chars` on a float. The result is correctly rounded and matches `std::from_chars<float>` bit for bit. `decoder_floats_match_from_chars` checks 1,000,000 random bit patterns, each in shortest form and in 17-digit scientific form. Overflow (`1e39`) and underflow of a nonzero literal (`1e-50`) are `E_RANGE`. Zero, `-0` and `1e-45` parse. A non-number is `E_TYPE`.
- `readInt` requires an integer token (`E_TYPE` otherwise) and checks the range (`E_RANGE`). `readHex64` requires 16 lowercase hex digits, and the caller picks the error code.

### 3.5 readValue, by kind

- **EntityRef:** `null` or a hex id. Zero is `E_ID_INVALID`. A nonzero id becomes a `PendingRef`, patched in phase 2.
- **Bool, Int32, UInt32** (range checked), **UInt64** (hex string), **Float**, **String**.
- **Vec2 to Vec4, Quat, Mat4:** arrays of floats. A quaternion is read in glTF order `[x, y, z, w]`. A matrix is 16 floats, column-major.
- **AssetRef:** an object with a non-empty `path` and an optional `guid`.
- **Enum:** a value name. Anything else is `E_ENUM`, with the list of values and a suggestion.
- **Vector** is sized before its elements are decoded, so pending references into them stay valid. **Optional:** `null` resets it. **Variant:** a one-key object `{"alt": {...}}`; an unknown alternative is `E_TYPE` with a suggestion. **Struct:** `readStruct`.

### 3.6 readStruct

- The value must be an object. A member matches a field name or an alias. The member's own position is tried first, so canonical files never search.
- An unknown field is `E_UNKNOWN_FIELD`, with the field list in `expected` and a suggestion.
- A field given twice, or by both name and alias, is `E_DUPLICATE_KEY`. Seen fields are tracked in a 64-bit mask, or in a `vector<bool>` above 64 fields.
- After a field decodes cleanly, its limits are checked. A value outside them is `E_RANGE`.

### 3.7 Error codes

`errorCodes()` lists these 20 codes with their meanings. The table is part of the catalogue.

- **Files:** `E_IO`.
- **Parsing and document:** `E_PARSE`, `E_FORMAT`, `E_VERSION_UNSUPPORTED`, `E_COMPONENT_VERSION`, `E_WORLD_NOT_EMPTY`.
- **Values:** `E_UNKNOWN_FIELD`, `E_DUPLICATE_KEY`, `E_TYPE`, `E_ENUM`, `E_RANGE`, `E_NONFINITE`.
- **IDs and hierarchy:** `E_ID_INVALID`, `E_ID_DUPLICATE`, `E_PARENT_MISSING`, `E_PARENT_CYCLE`, `E_SINGLETON`.
- **References:** `E_ENTITY_NOT_FOUND`.
- **Limits:** `E_TOO_MANY_ERRORS`.
- **Warning:** `W_UNKNOWN_COMPONENT`.

## 4. The ECS side (`ohao/ecs`)

### 4.1 Parsing

`parseJson(text)` returns a self-contained document that owns a padded copy of the text. `JsonParser` is reusable. It keeps the simdjson parser, the padded buffer and the tree storage warm, and its document is valid until the next `parse()`. Scene decoding uses a `thread_local` `JsonParser`, and nothing in the decoded output points into the document. A parse failure is `E_PARSE` at the failing byte, with its line and column.

### 4.2 API

- `saveScene` returns the text. Pretty is the default, and the text ends with `\n`. It fails only with `E_NONFINITE` or `E_ENUM`, at `/entities/<n>`.
- `validateScene` runs phase 1 only and needs no World.
- `loadScene` returns a `LoadReport` or the diagnostics. `saveSceneFile` and `loadSceneFile` use UTF-8 with LF line endings, and their failures are `E_IO`. `diagnosticsJson` returns a compact array.

### 4.3 Save order

Entities are written in creation order (`CreationOrder::seq`). Each starts with `"id"`, then its saved components in registration order (Name, Transform, Parent, Light), then its preserved unknown components sorted by name. Entity references are written as ids, and a dead target is `null`. The header `components` map is sorted by name.

### 4.4 Two-phase load

**Phase 1** (`decode`) collects every error and warning.

- `format` and `version` are checked, and `entities` is required. `components` is optional. An unknown root member is `E_UNKNOWN_FIELD`, with a suggestion.
- A header version that differs from the engine's gives `E_COMPONENT_VERSION`. Blocks of that component are skipped. The other entities are still decoded.
- Each entity needs a unique, nonzero, 16-digit hex `id`.
- Each component block decodes into its own heap block. A repeated component is `E_DUPLICATE_KEY`, and a second singleton is `E_SINGLETON`. A runtime-only or nested type name is `E_UNKNOWN_FIELD`. An unknown name is preserved when its value is an object, and otherwise is `E_TYPE`.
- Every non-null reference must name an entity in the file. A missing Parent target is `E_PARENT_MISSING`, and any other is `E_ENTITY_NOT_FOUND`.
- Parent chains are walked. Each cycle is reported once as `E_PARENT_CYCLE`, at the member of the cycle that comes first in the file.

**Phase 2** runs only when phase 1 has no errors, and it cannot fail.

1. Entities are created in file order with `createWithId`, using the decoded Name and Transform. Missing components take their defaults.
2. Pending references are patched with `findById`.
3. Other saved components are emplaced through `ComponentOps::emplace`, which calls `emplace_or_replace`. The normal signals fire, so no special load path is needed.
4. Preserved components go into `PreservedComponents`.
5. Parent is applied last, in file order, through `World::setParent`.

`loadScene` requires an empty destination. Otherwise it returns `E_WORLD_NOT_EMPTY` and changes nothing.

### 4.5 Unknown and version-mismatched components

- **Unknown component:** kept as canonical compact JSON with its header version (1 when the header has no entry). It is written back after the known components, sorted by name. `LoadReport::preservedUnknown` counts them.
- **Near miss:** an unknown name within the suggest threshold of a registered component name draws `W_UNKNOWN_COMPONENT`. The component is still kept.
- **Runtime-only component** in a file, such as `WorldTransform`: `E_UNKNOWN_FIELD`, "is runtime state and is never saved".
- **Version mismatch** for a known component: `E_COMPONENT_VERSION`. Migrations come in step 4.

## 5. Dependencies

- **simdjson v3.10.1** (`external/cmake/simdjson.cmake`): the single-header amalgamation, linked privately by `ohao_ecs`.
- **fast_float v6.1.6** (`external/cmake/fast_float.cmake`): header-only, private to `ohao_reflect`. `decode.cpp` uses it.
- The public reflect headers still use only glm and std.

## 6. Tests

### 6.1 scene_tests: 23/23

The suite has 21 tests and 2 benchmarks. Step 3 adds 9 tests and `bench_scene_save_load_10k`.

- **t02, round trip:** byte-identical in both layouts, with every component equal field by field.
- **t03, random worlds:** three seeded worlds of 1000 entities (seeds 101, 202, 303). They have extreme floats, unicode and control characters in names, every light type, and hierarchy depth up to 32. Each round-trips byte-identically and bit-exactly, in both layouts.
- **t04, hierarchy and order:** world matrices are bit-identical after an update. Creation order and light order are kept.
- **t04, references before their target:** a child that names a later parent loads and follows it.
- **t06, error corpus:** 18 files, each with exact expected codes and paths (section 6.2). A failed load leaves the world untouched.
- **t06, world must be empty:** a load into a non-empty world fails with `E_WORLD_NOT_EMPTY`.
- **t06, cap at 100:** 150 bad lights give 100 errors and one `E_TOO_MANY_ERRORS`, and each error carries its suggestion.
- **t07, unknown components:** a file with an unknown component loads, and saves back byte for byte with its header version.
- **t07, unknown fields:** a misspelled field fails with `E_UNKNOWN_FIELD` and suggestion `color`, at line 2. A component name one typo from `Light` warns and is kept.

### 6.2 The error corpus

Each `tests/scenes/invalid/NN_*.ohscene` has a `.errors.json` with the expected codes and paths, and the line or suggestion where one is given. The files cover:

- syntax: 01, 16 and 17 (`E_PARSE`);
- format and header: 02, 03, 04, 05, 14 and 18 (`E_FORMAT`, `E_VERSION_UNSUPPORTED`, `E_COMPONENT_VERSION`, `E_TYPE`);
- identity: 07 (`E_ID_DUPLICATE`, line 6) and 08 (`E_ID_INVALID`);
- hierarchy: 09 (`E_PARENT_CYCLE`, lines 5 and 7);
- values: 06 (`E_ENUM` suggesting `Spot`, `E_UNKNOWN_FIELD` suggesting `intensity`, `E_RANGE`, `E_PARENT_MISSING`), 10 (`E_TYPE`), 11 (runtime component), 12 (`E_DUPLICATE_KEY`), 13 (`E_RANGE`) and 15 (shape errors).

### 6.3 reflect_tests: 25/25

Two tests are new: `pretty_single_pass_matches_pretty_print` and `decoder_floats_match_from_chars`. One benchmark is new: `bench_float_parse`.

### 6.4 Goldens

`tests/scene/golden/engine_catalogue.json` and `tests/reflect/golden/fixture_catalogue.json` carry the error table. They were regenerated for it, and the diffs are additive.

## 7. Verification

- `engine_tests` 58/58; `ecs_tests` 26/26; `runtime_tests` 21/21; `renderer_test` all pass.
- Physics 59/59; force generators 54/54; `check_scene_schema.py` 14/14.
- Render goldens 5/5 PASS, with unchanged tolerances. `check_validation.py` is clean.

Not run: the windowed `OHAO_VALIDATION=1 ohao_game` run. The session's tool permissions refused environment-prefixed commands.

## 8. Performance

Informational. Release build on Frank's laptop, with about ±30% run-to-run noise. Nothing is gated in this step.

The measured scene has 10k entities with Name and Transform (random rotations), a Parent on a third of them, and 100 Lights. Its pretty file is 2.9 MB.

| Operation | Time |
|---|---|
| Save, pretty | 8.6 to 11.1 ms |
| Save, compact | 4.8 to 5.3 ms |
| JSON parse, warm parser | 4.2 to 5.4 ms (6.6 to 6.9 ms cold) |
| Validate | 14.3 to 15.1 ms |
| Load, including world teardown | 17.5 to 18.3 ms |

Per operation, float formatting takes about 28 ns. Float parsing takes 47 ns with `std::from_chars` and 32 ns in the decoder.

Against the step-6 budgets for the about-5 MB bench10k scene (save at most 10 ms, load at most 20 ms, validate at most 10 ms), save is near its budget. Validate and load would each exceed theirs by roughly 2x when scaled to 5 MB. Candidates for step 6:

- a smaller `JsonNode` (48 bytes today);
- arena allocation for decoded values, instead of one heap block per component;
- building reference paths lazily;
- a faster shortest-float formatter;
- avoiding the full-document copy when the root and the entities array break.

Step 6 measures these and gates the budgets.

## 9. Spec adjustments made in this step

- The JSON tree and warm-parser description now match the code.
- fast_float is added to decision 32.
- The error table gains `E_IO`, `E_DUPLICATE_KEY`, `E_WORLD_NOT_EMPTY`, `E_ENTITY_NOT_FOUND` and `W_UNKNOWN_COMPONENT`.
- The rules for unknown, runtime and version-mismatched components are written down.
- Step 3 is marked done in the build order.

## 10. Deferred to later steps

- Migrations and upgrades, and the `tests/scenes/v1` corpus: step 4.
- Commands, query and the method table: step 5.
- Snapshots and the gated budgets: step 6.
- `MeshSource`, `MaterialOverride`, `Camera` and `Environment`, and their variant decoding in real scenes: steps 7 and 8.
- `--check-assets` and `E_ASSET_NOT_FOUND`: step 9.
