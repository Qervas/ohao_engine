---
module: agent
id: hash-diff
title: World hash, diff and determinism
standard: v2
---

## Two questions an agent asks after every step

An agent playing a world needs two cheap answers: *is this the same world as before?*
and *what changed?* Reading the whole world back to compare it would cost hundreds of
kilobytes of JSON per question. `world.hash` answers the first with 16 hex digits, and
`world.diff` answers the second with only the changed fields of only the changed
entities. Both rest on a stronger guarantee: in lockstep the same scene, seed, method
sequence and injected input give bit-identical world state, and deferred captures of
that state give bit-identical pixels.

## What the hash covers

`world.hash` covers the authoring state, exactly what a snapshot captures, read straight
from the live storages without copying values:

{{cite ohao/ecs/world_hash.hpp "It hashes what a snapshot (ecs/snapshot.hpp) captures, read straight from the live"}}

In order, it hashes the entity table (ids, creation sequence, parent rows, child rows),
then every saved component type in the snapshot's column order, and last the id
generator and the next creation sequence:

{{cite ohao/ecs/world_hash.cpp "for (const reflect::TypeInfo* t : snapshotTypes(types)) componentOps(*t)->hash(reg, rowOf, *t, h);"}}

{{cite ohao/ecs/world_hash.cpp "h.u64(table.idGen.counter);"}}

The order is the snapshot's row order, which is creation order, not the order of EnTT's
packed storage. Packed order depends on the pools' history (a destroy swaps the last
element into the hole), so two worlds in the same state could store their components
differently; the hash must not see that. The id generator is in the hash because it
decides the ids of entities created later: two worlds that look the same but would hand
out different ids are not the same world.

Runtime state is left out: `WorldTransform`, render slots and dirty tags are derived
from the authoring state, so hashing them would add nothing but cost. The frame number is
left out too, and `world.hash` reports it beside the hash, so "the same world at another
frame" stays a meaningful answer.

## Hashing values, not bytes

Components are fed field by field through their registered types, never as raw object
bytes:

{{cite ohao/reflect/hash.hpp "are fed field by field through their registered types, never as raw object"}}

Padding bytes are whatever the allocator left there, and a heap pointer differs run to
run, so hashing an object's bytes would make equal worlds hash differently. Floats count
by their bits, the same rule equality uses, so `-0` and `+0` differ. An entity reference
counts as the target's `EntityId`, not its handle, because handles are recycled and are
not part of a world's identity. A type whose bytes are exactly its fields, such as
`Transform`, is hashed as its bytes, the fast path.
`sp4_t12_value_hash_ignores_padding_and_counts_bits` pins all of this.

The function is FNV-1a over 8-byte words with a fold after each multiply:

{{cite ohao/reflect/hash.hpp "word per step makes it eight times faster than byte-wise FNV over the same data. A final"}}

Each word's step is a bijection of the running state, so two inputs that differ in a
single word always hash differently, and taking a word per step instead of a byte is what
keeps the bench10k hash (10,002 entities) under its 1 ms budget. It is a fingerprint for
comparing runs of one engine build, not a cryptographic hash.

## Where the hash is used

`world.hash` reports it as `{hash, frame}`. Recordings use it twice: a recording starts
with the world's hash, so a replay can refuse a world it was not recorded on, and every
60 steps it writes a checkpoint, so a replay that drifts fails at the step where it
drifted (see the time and input unit). The same function backs both, so a hash an agent
reads with `world.hash` can be compared with one a recording wrote.

`sp4_t12_hash_is_stable_across_save_and_load` saves a world, loads the file with the same
seed, and requires the same hash; it also requires that runtime state does not move it and
that another id seed does. `sp4_t12_hash_changes_on_any_authoring_change` makes one edit
at a time (a position by one ulp, a sign bit, a name, a component added and one removed,
a reparent, an entity created and one destroyed) and requires a different hash for each.

## What changed since a snapshot

`world.diff {snapshot, fields?, limit}` compares the live world with a named snapshot and
writes nothing. `ecs::diffWorld` finds each snapshot row's live entity first. For an
unchanged world, row `r` is at packed position `r`, so most rows need no lookup:

{{cite ohao/ecs/world_diff.cpp "if (ids && r < ids->size() && at<EntityId>(*ids, r).value == s.ids[r]) e = ids->data()[r];"}}

Rows with no live entity are `destroyed`. Live entities that no row claims are `created`,
listed in creation order. Then each saved component's column is compared against the
live storage, column by column, and the parents are compared. The result groups the
changes by entity, in the snapshot's row order:

{{cite ohao/ecs/world_diff.hpp "std::vector<EntityDiff> changed;    // entities in both, in the snapshot's row (creation) order"}}

The API turns each change into file-form paths with the old and new values,
`{"/Transform/position": [old, new]}`, or `"/Type"` for a component added or removed.
`fields` keeps only the listed types and fields, and `limit` caps each list with a
`more` count, so "what did my 600-step play do?" costs a few hundred bytes for a few
moved entities. `sp4_t12_diff_equals_a_diff_of_the_two_saves` checks it against the
ground truth: the diff must list exactly what differs between the two scene files saved
before and after.

## Deterministic deferred pixels

Bit-identical world state is half the contract. The other half is that a deferred
capture of that state gives the same pixels every time, which the capture tests and the
replay check (`e2e_mcp.py` captures before and after a replay and compares the PNG files
byte for byte) depend on. Step 1 of sub-project 4 made this hold. It seeded the SSAO
kernel from the renderer seed. It also fixed two real races. One was a post pass that
sampled the image the previous render pass had just written without a dependency to make
the write visible:

{{cite ohao/render/deferred/render_pass_base.hpp "gave a different image run to run)."}}

The other was TAA's history on its first frame, which read a matrix nobody had set yet.
`deferred_determinism_gpu_test` holds the result: deferred frames, post effects included,
bit-identical across fresh renderers, processes and repeated captures, and the deferred
goldens are exact (`max_abs_diff` 0). The path tracer is not in this contract: it drifts
by 1 LSB on a few pixels across sessions, and that is sub-project 2b's to fix.

:::why
A hash and a diff instead of reading state back keeps an agent's play loop cheap. An
agent that steps, hashes and compares pays 60 bytes per check rather than a world's
worth of JSON, and it learns *where* two runs part, not just *that* they do. Hashing
reflected values rather than memory makes the hash mean what the scene file means: two
worlds with the same hash save to the same file.
:::

:::key
`world.hash` is a 64-bit fingerprint of the authoring state: entity table, saved
components in creation order, id generator; field by field through reflection, so
padding, pointers and storage order never count. `world.diff` lists changed fields of
changed entities against a snapshot, with created and destroyed ids. Lockstep worlds and
deferred captures are bit-identical run to run.
:::

## Contracts

- Compare hashes only within one engine build. Adding a saved component type changes the
  hash of unchanged scenes.
- A diff's snapshot must come from a world with the same registrations.
- Runtime components and the frame number are not part of the hash. Read the frame from
  `world.hash`'s `frame`.
- Deferred determinism holds for deferred captures in lockstep. A realtime capture is the
  presented frame (`deterministic: false`), and path-traced frames have their own
  tolerance.
