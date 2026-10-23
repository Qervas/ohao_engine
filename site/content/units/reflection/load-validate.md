---
module: reflection
id: load-validate
title: Loading & validation
standard: v2
---

## All or nothing

A scene load either produces the whole world in the file or changes nothing. Phase 1 parses and decodes the entire document into inert typed values in a per-thread bump arena, plus a list of pending entity references. It checks every id, every reference target and every parent chain, and collects every error. Only when it finds none does phase 2 touch the world:

{{cite ohao/ecs/scene_io.cpp "// Phase 2: nothing below can fail; every id, reference and value was checked."}}

Phase 2 runs in five steps. It creates entities in file order with `createWithId`, from their decoded `Name` and `Transform`. It patches pending references through `findById`. It emplaces the remaining components through `ComponentOps::emplace`, which is `emplace_or_replace`, so the ordinary signals fire and downstream systems need no load path of their own. It stores unknown components in `PreservedComponents`. Last, it attaches parents in file order through the trusted `World::attach`, which skips the ancestor walk because phase 1 already rejected missing parents and cycles. The file carries no id-generator state; `scene.load` takes an optional `seed` for that.

The destination must be empty, and the check is broader than "no ids":

{{cite ohao/ecs/scene_io.cpp "if (dst.idCount() != 0 || dst.registry().storage<entt::entity>().in_use() != 0) {"}}

A scratch entity without an `EntityId` also gives `E_WORLD_NOT_EMPTY`. There is no merge mode.

:::why
Decoding straight into the world would leave half a scene behind when entity 9,000 names a missing parent. The rejected alternative was to build into a temporary world and swap it in. That would create every entity twice and fire construct signals into systems attached to a world that is about to be discarded. Inert values cost the arena and one move per component.
:::

## Two decoders, one verdict

Phase 1 has two implementations. The tree decoder parses into a `JsonDocument` (the strict RFC 8259 reader in `ecs/json_parse.cpp`) and walks it with `readStruct`, reporting as it goes. The streaming decoder, `decodeFast`, pulls tokens straight off the text with a `JsonCursor` and `readStructFast`, and builds no tree. It never reports. At the first thing the tree decoder would object to, it returns false, and `decode` clears the partial output and hands the same text to the tree:

{{cite ohao/ecs/scene_io.cpp "if (allowFast && decodeFast(text, r, out)) {"}}

Because only the tree path can produce diagnostics, the two paths cannot report differently. The real risk is the stream *accepting*, or decoding differently, something the tree rejects. So the stream gives up on all of these:

- a `version` token other than literally `1`, an unknown or repeated root member, or a `components` header placed after `entities`;
- a saved component whose header version is not the registered one;
- an entity whose first member is not a valid, unique, nonzero `"id"`;
- an unknown component name, or one given twice:

{{cite ohao/ecs/scene_io.cpp "if (k == known.size() || ((seen >> k) & 1u)) return false;   // unknown, or twice"}}

- a second singleton, an unknown or repeated field, a value outside its limits, an escaped member name, or more than 64 component types or fields;
- a vector whose elements hold entity references, since its pending reference pointers would dangle as it grows:

{{cite ohao/reflect/codec.hpp "// References into a growing vector would dangle: leave those to the tree decoder."}}

- a missing asset under `checkAssets`, an unresolved reference, or a parent cycle.

Component lookup starts just after the previous match, so a file in registration order matches on the first compare, though other orders still stream.

The agreement is tested. `readersDisagree` compares `validateScene` with `validateSceneReference`, which always takes the tree, and `loadScene` with `loadSceneReference`. It compares the rendered errors and warnings, the upgrade list, the load verdict and the compact save of each loaded world:

{{cite tests/scene/scene_tests.cpp "static std::string readersDisagree(std::string_view text) {"}}

The three `t21_*` tests feed it different inputs. The first is 25 random worlds in both layouts, where `decodeCounters` must show that both decodes streamed, plus every file under `tests/scenes`. The second is edge and random number tokens, in a field with limits and one without. The third is 3,000 randomly edited files; the codec-speed change record reports 433 of them still valid.

:::key
A file that is not clean is decoded twice, first by the stream until it gives up and then by the tree. One preserved unknown component, or one entity that lists a component before its `"id"`, sends every load of that file down the slow path. Only clean, current files get faster.
:::

## What a diagnostic carries

A `Diagnostic` has a stable `code` from the `errorCodes()` table, which is part of the catalogue. It has a JSON Pointer `path` with RFC 6901 escaping, such as `/entities/0/Light/intensty`, and the holding entity's id when there is one. Its byte offset becomes a 1-based line and byte column. It also carries `message`, `expected`, `got` and `suggestion`; `got` is the offending value rendered compactly and cut at 80 characters. Errors and warnings are stable-sorted by offset, so they read top to bottom.

Suggestions use an optimal-string-alignment distance, case-insensitive. Insertions, deletions, substitutions and adjacent swaps cost 1 each, and a candidate is accepted only within a length-scaled threshold:

{{cite ohao/reflect/decode.cpp "const std::size_t limit = std::max<std::size_t>(1, word.size() / 3);"}}

The candidates are field names, enum values, variant alternatives, root members and component names. The corpus file `06_agent_mistakes` shows the result. `"type": "Spto"` gives `E_ENUM` with suggestion `Spot`. `"intensty"` gives `E_UNKNOWN_FIELD` with suggestion `intensity` and the field list in `expected`. A negative `radius` gives `E_RANGE`, and a dangling `Parent` gives `E_PARENT_MISSING`.

The 101st error becomes one `E_TOO_MANY_ERRORS`. It stays last after sorting and sets `full()`, which the decode loops poll so that they stop early:

{{cite ohao/reflect/decode.cpp "void DecodeContext::error(std::string_view code, const JsonNode& at, std::string message, std::string expected,"}}

Warnings are not capped. `scene.validate` trims both lists to its `limit` (default 20, maximum 100) and reports the count it cut as `more`.

## Numbers

A float goes straight from its token to a correctly rounded `float` through `fast_float`, never via `double`. Overflow (`1e39`) and a nonzero literal that underflows to zero (`1e-50`) are both `E_RANGE`. JSON cannot spell NaN or infinity, so `NaN` is `E_PARSE` and no non-finite value enters through a file. `E_NONFINITE` is the save-side error for a NaN already in the world. Integer fields demand an integer token, so `"version": 1.0` is `E_TYPE`. Registered limits are checked after a clean decode, so `"innerConeDeg": 181` is `E_RANGE`, with the limits written out in `expected`.

Validation keeps no values, so it skips the conversion when the verdict does not need it:

{{cite ohao/reflect/decode.cpp "if (!ctx.needValue && floatFitsQuick(n.text)) return true;"}}

`floatFitsQuick` judges from the decimal exponent of the leading digit. Anything from 10^-44 to 10^37 certainly fits. Tokens near either end, or with exponents longer than three digits, are converted. Fields with limits always convert, because the limit test needs the value. `t06_validate_reports_what_load_reports_for_float_edges` checks that validate and load agree on tokens such as `3.4028236e38` and `7e-46`.

## Versions and upgrades

The document `version` must be the integer 1, or the file gets `E_VERSION_UNSUPPORTED`. The optional `components` header records the version each component was written with; a missing entry means current. When the two differ:

{{cite ohao/ecs/scene_io.cpp "if (fileVersion < t->version && t->canUpgradeFrom(fileVersion)) {"}}

An older version with a complete upgrade chain is upgraded. The block is copied into a mutable `JsonValue`, each step up to the current version runs on it, and the result is decoded by the ordinary `readStruct`, so upgraded data passes the same checks as current data. A step that rejects the data gives `E_UPGRADE` with its reason. A newer version, or an older one with a gap in the chain, gives `E_COMPONENT_VERSION` at `/components/<Name>`, with the readable versions in `expected`. All of this is phase 1. No engine component is above version 1 yet, so only test components such as `TestLamp` exercise this path.

## Unknown components, unknown fields

An unknown *component* is kept: stored as compact JSON with its header version and written back on save. It draws `W_UNKNOWN_COMPONENT` only when its name is a near miss of a known one:

{{cite ohao/ecs/scene_io.cpp "const std::string near = reflect::suggest(c.key, knownNames);"}}

An unknown *field* in a known component is always `E_UNKNOWN_FIELD`. A component the engine does not know may belong to a newer engine or another tool, and keeping it is free. A field the decoder cannot place in a type it does know is a mistake, and accepting it would drop the field on the next save. A runtime-only name such as `WorldTransform` is also `E_UNKNOWN_FIELD`.

## Validate is load without the world

`validateScene` runs the same `decode`, decoding each component into one reused scratch object per type. It touches no world and returns the verdict, diagnostics and upgrade list that a load would. With an `AssetResolver` (`checkAssets`), a `ModelAsset` or `Environment` HDRI whose file is missing is `E_ASSET_NOT_FOUND`.

## Sharp edges

- Root-level errors stop the decode before `entities`. A file with `"version": 2` reports only that, and fixing it can reveal more errors.
- `E_COMPONENT_VERSION` skips every block of that type, so errors inside those blocks stay hidden until the version is fixed.
- An error found after an upgrade keeps its field path, but its line points at the start of the component block.
- `checkAssets` exists only in validation, and `loadScene` takes no resolver. A missing model loads cleanly and surfaces later as an `InstantiateError` component, when instantiate runs.
