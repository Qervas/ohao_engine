---
module: reflection
id: api
title: Session, commands & queries
standard: v2
---

## One function, text in and text out

Every authoring operation in OHAO is a method name plus a JSON params object, answered by one function. `api::Session` holds a `World&` and the `TypeRegistry`; it owns nothing else that matters, apart from named snapshots and two optional hooks (an asset resolver and a render handler):

{{cite ohao/api/session.hpp "[[nodiscard]] std::string call(std::string_view method, std::string_view params = {});"}}

The signature is untyped on purpose. The `ohao` CLI, its script lines, the planned `ohao serve` JSON-RPC/MCP server and the planned C ABI all pass a method string and a params text and print what comes back, so each operation has one decoder, one set of limit checks and one error shape. The alternative is a typed C++ function per operation plus a JSON wrapper per transport. That puts validation and error formatting in every wrapper, and the wrappers drift apart. The cost is one parse per call. The params parser is a session member that keeps its buffers between parses. The step-9 record measures a 100-command `world.apply` batch on the 10k-entity bench world at a 0.182 ms median, against a 0.2 ms budget.

## The method table and the error envelope

Fourteen methods live in two tables: `buildMethods` (name, params struct, result, doc), exported as the catalogue's `methods` table and hashed into `schemaHash`, and `kDispatch` (name, function pointer). `api_tests` checks that every documented name dispatches and has a params struct.

Each method's params are a struct registered in its own registry (`ohao/api/params.hpp`), so one definition is the validation, the catalogue's JSON Schema and the MCP tool's input schema. A method starts with `decodeParams`, the typed struct decoder of scene files (MCP's `_meta` skipped):

{{cite ohao/api/internal.hpp "reflect::readStruct(&out, *t, params, ctx,"}}

Unknown members, types, enums and limits are reported exactly as in scene files (`E_UNKNOWN_FIELD` with a suggestion, `E_RANGE` with the limits). Errors found there, or by a rule on the params alone, mark the call `BadParams`, which the protocol server answers with JSON-RPC's -32602; anything later is a domain error.

A reply is exactly one of `{"result": …}` or `{"error": {"code", "message", "data"}}`. `call` opens the envelope, writes the result straight into it, and throws the writer away if the method fails. The writer is given the world's id mapper, so entity references go out as 16-hex-digit stable ids, never as `entt::entity` values. On failure, `code` and `message` come from the first diagnostic, with ` (+N more)` appended, and `data` carries at most 20 diagnostics, each with a JSON-pointer path into the params (`/commands/4/value`):

{{cite ohao/api/session.cpp "constexpr std::size_t kMaxErrorData = 20;   // diagnostics per error envelope"}}

An unknown method is `E_METHOD` with an edit-distance suggestion. Every method first checks its member names, so `limt` is `E_UNKNOWN_FIELD` at `/limt`, with the suggestion `limit`.

The table is the only description of a method's params and result that agents see, so it has to match the code; nothing checks that mechanically. One row drifted until step 10: `world.summary` documented a `truncated?` member that `methodSummary` never writes. It now names what the code writes — `moreRoots` at the top level and `more` on outline nodes — and the fix changed `schemaHash`, as any table edit does:

{{cite ohao/api/session.cpp "{entities, roots, components, bounds?, outline, moreRoots?}"}}

## A batch is atomic without a snapshot

`world.apply {commands}` runs seven ops in order: `entity.create`, `entity.destroy`, `entity.reparent`, `component.set`, `component.remove`, `field.set` and `entity.duplicate`. Each command is checked against the world as the earlier commands left it, so a batch can create an entity and then edit it. That ordering is a revision. The step-5 record says the first design validated the whole batch before applying any of it, and that design could not check a command against an entity an earlier command creates, reparents or destroys.

Inside a command, everything is checked before anything changes. `entity.create` decodes every component and preserved value and resolves every entity reference, and only then reaches the line after which nothing can fail:

{{cite ohao/api/commands.cpp "const uint64_t seq = world.nextCreationSeq();"}}

The main run always records each command's inverse, because the inverse is part of the result. So rollback costs only a replay. When command *i* fails, the inverses of commands 0 to *i*−1 are joined last-first, parsed with a second parser, and run with inverse recording off. Then two counters that the replay cannot undo are put back:

{{cite ohao/api/commands.cpp "s.world.setNextCreationSeq(nextSeq);"}}

Generated ids are `splitmix64(seed + counter++)`, so creates that were rolled back still advanced the counter. Without restoring the generator, the next id after a failed batch would differ from a world where the batch never ran. `t09_failing_command_rolls_back_the_batch` checks both: after a five-command batch fails on its last command, the save is byte-identical, and the next generated id equals a fresh world's. If the replay itself fails, a second `E_COMMAND` diagnostic says the world may be partially edited.

## Inverses, and what "byte-identical" covers

| Command | Its inverse |
|---|---|
| `entity.create`, `entity.duplicate` | `entity.destroy` of the new entity |
| `entity.destroy` | one `entity.create` per destroyed entity, in pre-order, each carrying its id, `order`, file-form components and preserved unknown components; plus reparents of children that a non-recursive destroy detached |
| `entity.reparent` | a reparent back to the old parent |
| `component.set` | the old value, or `component.remove` if there was none |
| `component.remove` | a `component.set` of the old value |
| `field.set` | the old value at the same path |

The restore-only `order` member puts back an entity's `CreationOrder` without taking a new sequence number:

{{cite ohao/api/commands.cpp "world.setNextCreationSeq(seq);   // a restored entity takes no new sequence number"}}

A batch's inverse is its commands' inverse lists, last command first, and the inverse of the inverse redoes the batch. `t09_batch_then_inverse_is_byte_identical` runs this over 200+ random commands. "Byte-identical" means the save. `World::setParent` appends a child to its parent's `Children`, so an undo that recreates a middle child changes the in-memory sibling order. The save does not change, because it orders entities by `CreationOrder`, which the undo restores. An undo is an ordinary batch, so it does not rewind the id generator; only a rollback does.

## `$k` and `entity.duplicate`

The command-level `id` and `parent` members accept `"$k"`, meaning the entity created by command *k* of the same batch. *k* must be earlier than the current command and must have created something. Both `entity.create` and `entity.duplicate` count. Entity references inside component values are ordinary hex ids, resolved against the live world.

{{cite ohao/api/commands.cpp "ok = r.ec == std::errc{} && r.ptr == n.text.data() + n.text.size() && n.text.size() > 1 && k < b.index &&"}}

`entity.duplicate` collects the subtree in pre-order, in child order (or only the entity with `recursive: false`). It refuses before changing anything in two cases: the target parent lies inside the copied subtree (`E_PARENT_CYCLE`), or any copied entity holds a singleton type such as `Camera` or `Environment` (`E_SINGLETON`). Each copy gets a fresh generated id and a new creation order. The root takes the optional new `name`; descendants keep their names. Each copy gets its source's local `Transform`, every saved component and its preserved unknown components. Hierarchy is remapped onto the copies, but other entity references are copied as they are and keep their targets. Each value is copied out before it is emplaced:

{{cite ohao/api/commands.cpp "OwnedValue copy(t, v);"}}

The copy is not about pool growth — EnTT's default storage is paged (1024 elements per page), so an insert does not move existing values. It is because `ComponentOps::emplace` *moves* from the value it is given, and `tryGet` returns the source entity's live component: passing it straight through would empty the source's strings and vectors.

{{cite ohao/api/commands.cpp "// ComponentOps::emplace moves from its argument, which would empty the source's"}}

## Queries: small answers about a large world

`world.query` filters, and every given filter must match: `ids` (any of; ids that do not exist are listed in `missing`, not reported as errors), `names` (exact, any of), `nameContains` (substring) and `has` (every listed component, runtime ones included). Matches come in creation order. `limit` (default 50) and `offset` page them, `count` is the total number of matches, and `next` appears only when more remain.

The projection decides how much comes back. Without `fields`, each entity is its compact file form. `"/Type"` gives a whole component with default-valued fields left out unless `defaults: true`. `"/Type/field"` gives that field even when it equals its default, because it was asked for by name. `fields: []` gives ids only. A path deeper than a field is refused:

{{cite ohao/api/query.cpp "projections stop at a field"}}

`world.summary` returns entity and root counts, the count of each component type, the bounds of entity origins and a hierarchy outline capped by `depth`, `maxChildren` and `maxNodes`. The bounds are composed from local transforms, so they are correct before the first step. This is the agent's token budget. The step-5 record measured a 10k-entity summary at 3.3 KB (about 830 tokens), a one-field query at 61 + 98 bytes, and the whole compact scene at 1.26 MB. An agent orients with the summary and then reads only the fields it needs.

## Time and pixels

Time is a `Timeline` (`core/timeline.hpp`): a mode, a pause flag, a fixed step and the frame number, the one time axis every step is counted on. A session owns one until a window host shares its own, so the host's realtime steps and the agent's `world.step` count on the same frames. Lockstep advances only on request; realtime steps from wall time and, paused, behaves as lockstep. Only a window host can run realtime:

{{cite ohao/api/session.cpp "if (p.mode == TimeMode::realtime && s.host != HostKind::Window) {"}}

`world.step {frames = 1, dt}` runs the world's simulate phase once per frame (`dt` defaults to the timeline's fixed step), then its sync phase once, and is `E_STATE` while the clock runs by itself. `watch` returns fields after the last step through the same selection and projection as `world.query`, checked before any step runs, so a bad path costs no simulation. `world.restore` resets the frame to the snapshot's.

`world.hash` is 64 bits over everything a snapshot holds, read live in creation order without copying the values; field by field, so padding and heap pointers never reach it, and a type whose bytes are exactly its fields (such as `Transform`) as its bytes:

{{cite ohao/reflect/registry.cpp "plain = t.triviallyCopyable && t.size % 4 == 0 && t.size > 0 && plainFieldsSize(t) == t.size ? 1 : 0;"}}

`world.diff {snapshot, fields?, limit}` compares the live world with a named snapshot, column by column and read-only (a memcmp first for trivially copyable components), and reports per entity `"/Type/field": [old, new]` in file form, `"/Type"` for a component added or removed, and the created and destroyed ids. It reports what a diff of the two saves would.

`render.capture` is checked entirely in the session: the path, the mode and denoiser names, `spp`, `frames`, sizes from 16 to 8192, each `post` effect name, and exposure in (0, 64]. Only then does the session look for a handler:

{{cite ohao/api/session.cpp "this session has no renderer; render.capture needs a host with a GPU (ohao run)"}}

The work is done by whatever `setRenderHandler` installed, a `std::function<RenderOutcome(const RenderRequest&)>`, and the outcome's diagnostics become the reply's. `ohao_api` therefore never links the renderer. In the API tests, malformed captures fail before the fake host is ever called.

## Contracts

- A command must finish all its checks before its first change. Rollback replays the inverses of the commands *before* the failing one; a command that changes the world and then fails is never undone.
- A method that returns false must report a diagnostic. Otherwise the envelope makes up an `E_COMMAND`, "the request failed without a diagnostic".
- `buildMethods` and `kDispatch` must list the same methods; `api_tests` checks it. The catalogue and `tools/list` follow `buildMethods` order.
- The `Session` does not own the world, the registry or the resolver. All three must outlive it.
