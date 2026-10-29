---
module: reflection
id: scene-files
title: Canonical scene files
standard: v2
---

## The shape of a file

A scene is a JSON object with four members: `"format": "ohao.scene"`, a document `"version"` (only 1 is accepted; anything else is `E_VERSION_UNSUPPORTED`), a `"components"` header, and the `"entities"` array. Each entity is an object whose first key is `"id"`, 16 lowercase hex digits. The Cornell box's environment entity is one line of the checked-in file:

{{cite assets/scenes/cornell_box.scene.json "1a0edc87612c3fd5"}}

After the id come the entity's saved components in registration order (`Name`, `Transform`, `Parent`, `Light`, `MeshSource`, `Material`, `Camera`, `Environment`, `Spin`), then any unknown components the entity kept from a file, sorted by name. The header maps each component name the file uses to the version it was written at, so a newer engine knows what to upgrade. Unlike everything else in the file, the header is in alphabetical order, because the save builds it in a `std::map`:

{{cite ohao/ecs/scene_io.cpp "std::map<std::string, uint32_t> header;   // sorted by name"}}

That is why the Cornell header reads `Camera, Environment, Light, ...` while each entity's blocks start with `Name, Transform`.

## Ids that survive a reload

An `entt::entity` is an index plus a version that the registry recycles, so its value depends on the history of creates and destroys in one process. A file, a command or a diff needs a key that names the same entity in another world on another day. `EntityId` is a 64-bit value; 0 is invalid. `World::create` generates one:

{{cite ohao/ecs/world.cpp "const uint64_t id = splitmix64(m_idGen.seed + m_idGen.counter++);"}}

splitmix64 is a bijection on 64 bits, so for a fixed seed, consecutive counters never produce the same id before 2^64 creates. The loop skips 0 and any id already claimed, and each skipped candidate still consumes a counter step. The whole generator state is the pair (seed, counter):

- `seedIds(s)` sets it to (s, 0). Without it, the seed comes from `std::random_device`.
- `idGenState` and `setIdGenState` save and restore it. Restoring the pair repeats the future ids, as long as the same ids are in use, because the skips depend on that set.
- A destroyed entity's id is never generated again, since the counter has moved on, but it can be claimed explicitly with `createWithId`.

Load creates every entity with `createWithId` in file order, so `CreationOrder` is renumbered to match the file, and a re-save writes the same order. Entity references are decoded as ids and patched only after every entity exists, so a parent may appear after its child:

{{cite ohao/ecs/scene_io.cpp "ref.assign(ref.field, reflect::ValueTraits<entt::entity>::toRaw(dst.findById(ref.id)));"}}

The file does not record generator state. A world that loads a file and then creates entities draws ids from its own seed, which differs on every run unless the caller seeds it. The examples fix their seeds (`kCornellBoxIdSeed`), and `scene.load` takes an optional `seed` that it applies after the load.

## One canonical form

**Defaults are left out, compared bit for bit.** A field is omitted when it equals the same field of the type's default-constructed instance, and float equality compares bits:

{{cite ohao/reflect/codec.hpp "return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);"}}

So `-0` is not the default `0`: it is written as `-0` and reloads as `-0`. Omission works per field, so a `vec3` is written whole when any one of its components differs. A test pins the result: an entity with an empty name at `(-0, 0, 0)` saves as

{{cite tests/scene/scene_tests.cpp "position":[-0,0,0]"}}

`Name` and `Transform` are always present on every entity, so when they are entirely default they are dropped as whole blocks. Any other component, even when entirely default, is still written as `{}` (the Cornell `"Environment": {}`), because its presence is the information. Values nested in variants and optionals are written in full. `SaveOptions::includeDefaults` writes every field instead; loading that and saving again gives back the omitted form.

**There is one pretty layout.** It uses a 2-space indent. A container stays on one line when it is empty, or when it is not the root and its one-line form, plus a trailing comma, ends by column 100:

{{cite ohao/reflect/json_layout.cpp "if (n.kind == NodeKind::Leaf || n.count == 0 || (!root && column + n.oneLine + (trailingComma ? 1u : 0u) <= width))"}}

The column includes the indent and the key. One-line objects pad their braces (`{ "a": 1 }`) and arrays do not. The compact rendering has no whitespace at all, and the pretty file is exactly `prettyPrint(compact)` plus a newline, which `t02_round_trip_is_byte_identical` asserts.

:::why
One canonical form makes save, load and save byte-identical (t02, and t03 on random 1000-entity worlds with extreme floats and unicode names). A changed field is then a one-line diff, for a person reviewing a commit and for an agent that edits a file and saves it again. Permissive output (key order from a hash map, `1.0` in one place and `1` in another, default fields appearing as they are touched) would churn bytes that carry no change. Canonical order also pays off on load: the streaming decoder accepts only clean files with members in canonical order, and anything else falls back to the slower tree decoder.
:::

## Two writers, one output

`saveScene` writes each authoring component with a hand-written `writeFields` overload. The keys are string literals and there is no reflection walk. The writers fill a per-entity `LayoutTree` whose nodes know their one-line lengths before anything is emitted, so each byte is written once; entities stream out once the save knows whether the `"entities"` array fits on a line. `saveSceneGeneric` is the reference: `writeStruct` into a `JsonWriter`, which writes one-line forms and re-lays out each container that overflows. Test 21 holds the two together, byte for byte, in pretty and compact, with and without defaults:

{{cite tests/scene/scene_tests.cpp "const auto b = saveSceneGeneric(w, engineTypes(), o, &rb);"}}

The with-defaults pass matters. The per-type writers duplicate the registrations by hand (the `Light` writer even spells out the four `LightType` names), and with every field written, a field added to a registration but missing from its writer fails the test whatever its value. The codec-speed record measured `saveScene` (pretty) on bench10k, 10,002 entities and 5.13 MB, at 8.98 to 9.18 ms against a 10 ms budget, down from 14.3 to 14.5 ms before the rewrite.

## Shortest floats, matched to to_chars

A float is written in its shortest round-trip form, byte for byte what MSVC's `std::to_chars(float)` prints. Whole numbers below 10^4 take a fast path. Everything else goes through Schubfach (`shortestDecimal`), and then `writeShortestOf` chooses between fixed and scientific notation the way `to_chars` does: fixed unless scientific is strictly shorter. One quirk had to be copied. When fixed notation wins and the shortest digits end before the decimal point (integers of 2^24 and up), `to_chars` prints the float's exact integer value, not the digits padded with zeros:

{{cite ohao/reflect/json_writer.cpp "if (fixedLen <= sciLen && exponent > 0 && value >= 0.0f)"}}

The record's example is `50331648`, not `50331650`. Scenes repeat values, so a 512-entry direct-mapped memo per thread, keyed on the float's bits, returns a repeated value's text without formatting it again:

{{cite ohao/reflect/json_writer.cpp "Entry& e = memo[(bits * 0x9E3779B1u) >> 23];"}}

Every test run compares 32M sampled floats with `to_chars`. The codec-speed record ran all 4,278,190,080 finite floats (`reflect_tests --exhaustive-floats`) with 0 mismatches. A NaN or infinity cannot be written: the save fails with `E_NONFINITE` and the entity's path.

## What a file keeps, and what it drops

Unknown components are kept. When a load meets a component block it does not know, it re-serializes the block as compact JSON and stores it in `PreservedComponents`, and the save writes it back after the known blocks:

{{cite ohao/ecs/scene_io.cpp "tree.compact(scratchKey, entry->json);"}}

Only whitespace and string escaping are normalized. Number tokens and member order inside the block are kept verbatim. Each preserved entry records the version from the file's header, but the header holds one version per name. If two entities carry the same unknown component at different versions (`entity.create` accepts a `preserved` block with any version), the header keeps whichever entry the save meets first, and on reload both entries take that version. The two writers do not even meet them in the same order: `saveSceneGeneric` walks creation order, while `saveScene` walks the `PreservedComponents` pool.

Some things are dropped, with or without a warning:

- **A mesh made in code** (`spawnModel`) has a `MeshRenderer` but no `MeshSource`, so the file cannot rebuild it. The save still succeeds. `W_UNSAVEABLE_MESH` is reported only to a caller that passes a `SaveReport`:

{{cite ohao/ecs/scene_io.cpp "if (!reg.all_of<MeshRenderer>(e) || reg.all_of<MeshSource>(e)) continue;"}}

- **Entities that lack `EntityId` or `CreationOrder`**, for example ones created with `registry().create()` instead of `World::create`, are not in the save's view and are skipped silently.
- **A failed file write.** `saveSceneFile` opens the target with `trunc` and writes it in place, with no temporary file and no rename. A value that cannot be written fails before the file is opened, but a failed write (a full disk, say) leaves a truncated scene behind.
