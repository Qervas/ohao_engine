---
module: reflection
id: registry
title: Type registry & schema
standard: v2
---

## A registry written out by hand

Scene files, validation, commands, snapshots and the agent catalogue all need to know what a component is, field by field. That knowledge lives in a `reflect::TypeRegistry`, an ordinary object filled by `ecs::registerSceneTypes`. There is no global instance and no static registration. Each field is one call in a builder chain, a template over the member pointer:

{{cite ohao/reflect/registry.hpp "f.ops = std::make_unique<MemberOps<T, M>>(member);"}}

`MemberOps<T, M>` implements `FieldOps` (write, tree read, streaming read, bit-exact equality, limit check, address) by calling `writeValue<M>`, `readValue<M>` and so on directly. A field operation is therefore one virtual call into code compiled for the member's exact C++ type, with no switch over field kinds. `kindOf<V>()` maps the member type to one of 18 kinds and `static_assert`s on anything it cannot serialize, so an unsupported member fails to compile at its registration line.

The design record states the choice (decision 1: own registry, member-pointer registration) without arguing against named alternatives, but the registrations show why the struct alone could not be enough. Most of a registration is information the C++ type does not contain:

{{cite ohao/ecs/scene_types.cpp ".field("intensity", &Light::intensity).doc("Brightness multiplier on color")"}}

That line goes on to say the value is dimensionless, has a hard minimum of 0 and an editor range of 0 to 100. Automatic aggregate reflection could enumerate `Light`'s members, but it could not supply any of that.

## What the hand-written chain gives up, and the test that buys it back

Nothing ties the chain to the struct. Add a member to `Light` and forget the `.field` call, and the member is never saved or loaded, with no warning. Boost.PFR is used for the one thing it does well, counting an aggregate's members, and it is linked into the tests only:

{{cite tests/scene/scene_tests.cpp "const bool ok = t->fields.size() + t->skipped.size() == boost::pfr::tuple_size_v<T>;"}}

Each member must be either a field or an explicit `.skip(name, &T::m, reason)`, and a skip needs a non-empty reason that the catalogue publishes. The test lists types by hand, and it also checks `checked == r.types().size()`, so a registered type cannot be left out of the list. It has two blind spots. It compares counts, not identities: a member registered twice under two names, with another one forgotten, still balances. And a component struct that was never registered at all is invisible to it.

## What a registration carries

A `TypeInfo` holds the name, doc, version, role flags, the fields in registration order (also the key order in files), the skipped members, the upgrades, and what reflection needs to treat the type as raw memory: size, alignment, construct, destroy, copy, and a shared default instance. Per field, `FieldAttrs` carries:

- `doc` and `tooltip`.
- `unit`, which must come from a fixed vocabulary, or `unitless()` for an explicitly dimensionless value. The vocabulary is 19 names, from `m`, `rad/s` and `deg/s` to `linear-rgb`, `lux` and `nits`:

{{cite ohao/reflect/registry.cpp "constexpr std::array<std::string_view, 19> kUnits = {"}}

- `min` and `max`, which are hard limits enforced by validation and commands, per component for vectors. `uiRange` and `step` are editor hints and are not enforced.
- `color()` (only on vec3 and vec4), `angle()` (only on float), `hidden`, `readOnly`, and `alias`, an older name that is still accepted on load.

Roles are flags on the type:

- `component()` makes the type a top-level block in an entity. Without it, the type is a nested value, such as the `*Prim` structs inside `MeshSource`.
- `runtime()` puts the type in the catalogue as `serialized: false`, and it is never saved. `WorldTransform`, `MeshRenderer` and the dirty tags are runtime.
- `singleton()` (`Camera`, `Environment`) means at most one per world. The scene decoder reports `E_SINGLETON`, and commands refuse a second one. `World` does not enforce it, so `registry().emplace<Camera>` on a second entity succeeds.
- `alwaysPresent()` (`Name`, `Transform`) marks a component that every entity has. It is left out of files when it is entirely default, and the remove command refuses it.

The version starts at 1, and version N needs `.upgrade(v, fn)` for every v < N. The catalogue lists the versions a type can read as `readsVersions`. Every engine type is at version 1 today, so only the test fixtures register upgrades.

The chain's grammar has one trap: an attribute call binds to the most recent field, or to the type when no field has been added yet. `.doc()` written before the first `.field` documents the type, and the same call written after it documents the field.

## What the registry refuses

Builder mistakes are recorded in `errors()` rather than thrown. They include names that are not identifiers, duplicate type, enum, field or alias names, a C++ type registered twice, a field whose nested type is not registered yet (nested types must come first), a unit outside the vocabulary or on a non-numeric field, `min > max`, `uiRange` with lo > hi, a step that is not positive, an upgrade outside `[1, version)`, and a default that its own limits reject:

{{cite ohao/reflect/registry.hpp "default value is outside [min, max]"}}

That last check runs against the default-constructed instance, so a type's defaults can never fail the type's own validation. `lint()` adds the rules agents depend on: docs on every type, field, enum and enum value; no empty enums; no serialized component without fields; an upgrade for every older version; and a unit or `unitless()` on every numeric field of a serialized type:

{{cite ohao/reflect/schema.cpp "numeric field needs .unit() or .unitless()"}}

:::key
No engine code calls `lint()` or `errors()`. The only gate is `t01_registrations_lint_clean`. If a field is rejected in a build that skipped the tests, the field is simply absent from the registry: its member is never written or read, and nothing at runtime says so.
:::

## Type-erased component operations

Reflect never includes EnTT. Two seams connect it to the ECS. The first is `ValueTraits<entt::entity>`, specialized in `scene_types.hpp`, which turns entity handle members into `EntityRef` fields that files write as the target's id. The second is `TypeInfo::host`, an opaque pointer that the ECS fills with a `ComponentOps` built by `makeComponentOps<T>()`:

- `emplace`, which calls `emplace_or_replace` and so fires EnTT signals.
- `tryGet`, `remove` and `count`.
- `capture` and `restore` for snapshot columns.
- `pool` and `tryGetIn`, which resolve the storage once per pass rather than once per entity.
- `writeFields`, the per-type scene writer, found by a requires-expression:

{{cite ohao/ecs/scene_types.hpp "if constexpr (requires(const T& v, FieldSink& s) { writeFields(v, &v, s); }) return &writeFieldsErased<T>;"}}

If no overload is visible, `writeFields` is null and the save falls back to the generic reflection writer. Tag types carry no ops. Nothing requires the ops, either: a serialized component registered without `.host(...)` is quietly left out of every save:

{{cite ohao/ecs/scene_io.cpp "if (const ComponentOps* ops = componentOps(*t)) out.push_back({t, ops});"}}

On load, its block is rejected as "not a component (it is a nested value type)". Lint cannot see this, because reflect never looks inside `host`. For engine types, `t21_every_authoring_component_has_a_writer` catches it.

## Two exports and one hash

`exportCatalogue` writes `ohao_schema.json`: the unit vocabulary, the error-code table, the API method table, and every enum and type in registration order with every attribute, its default and its skipped members. A field shows unit `"1"` when it is `unitless()` and `null` when it has no unit. `schemaHash` is a 64-bit FNV-1a of the compact catalogue body, without the `schemaHash` member itself:

{{cite ohao/reflect/schema.cpp "return fnv1a64(w.str());"}}

Every byte of that body counts, so rewording a doc string changes the hash just as a new field does (`t12_catalogue_is_deterministic_and_hashed` tests exactly that). An agent comparing hashes sees any edit to the registry as a changed engine.

`exportSceneJsonSchema` writes a draft 2020-12 schema for `*.scene.json` from the serialized types only:

- Known structs set `additionalProperties: false`, and unknown component blocks are accepted as opaque objects.
- Ids must match `^(?!0{16})[0-9a-f]{16}$`.
- Limits become `minimum` and `maximum`, clamped to the int32 or uint32 range. Quat and mat4 drop theirs.
- Aliases appear as deprecated properties.
- Cross-entity rules (unique ids, parents, cycles, singletons) stay with the engine's validator.

There is a sharp edge between the two exports. `ohao schema` writes the catalogue with the API method table:

{{cite tools/ohao/main.cpp "reflect::exportCatalogue(engineTypes(), {api::methods(), reflect::JsonLayout::Pretty});"}}

The scene schema, however, embeds a hash computed with no methods:

{{cite ohao/reflect/schema.cpp "h.hex64(schemaHash(r));"}}

So one `ohao schema` run writes two files that name different hashes: the catalogue's covers the method table, the scene schema's does not (a scene file does not depend on the API). A cache keyed on one cannot be checked against the other.

## Never on a frame

`t13_no_reflection_on_hot_paths` scans every file under `ohao/render` and `ohao/gpu`, plus the transform, render-extract, instantiate and spin system sources, for an `#include` naming `reflect/` or `api/`:

{{cite tests/reflect/reflect_tests.cpp "for (const char* dir : {"ohao/render", "ohao/gpu"})"}}

A second test holds `ohao/reflect` to glm and the standard library, with no EnTT, Vulkan, `core/`, `gpu/`, `render/` or `ecs/` includes. Both tests match include lines as text and do not follow includes. A system source that included `ecs/scene_types.hpp` would pull in the whole registry and still pass. Inside `ohao/`, only `scene_types.cpp`, `scene_io.cpp`, `snapshot.cpp` and `api/internal.hpp` include that header today.
