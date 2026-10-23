---
module: reflection
id: snapshots
title: Snapshots & restore
standard: v2
---

## The loop that must not touch text

An agent working a scene runs the same cycle over and over: snapshot, try an edit, step, look, restore. Doing that with scene text would mean a full save followed by a full load. The load needs an empty world, so every entity would be destroyed and rebuilt, and every render slot with it. The step-6 change record measured that pair at 8.3 ms (save) plus 11.4 ms (load) median on its 10,000-entity bench. In the same run, `takeSnapshot` took 0.34 ms and an unchanged `restoreSnapshot` took 0.34 ms. A snapshot is an in-memory, columnar copy of the authoring state, and restore diffs the live world against it instead of rebuilding.

## What a snapshot holds

`Snapshot` covers every entity that has an `EntityId`, one row per entity, rows in creation order:

- `ids` and `order`: each row's `EntityId` and its `CreationOrder` sequence;
- `parent`: the parent's row, or `kNoRow`;
- the children of each row, in `Children` order, in compressed-row form:

{{cite ohao/ecs/snapshot.hpp "std::vector<uint32_t> childBegin;  // children of row r: children[childBegin[r] .. childBegin[r + 1])"}}

- one `SnapshotColumn` per saved component type, plus one for `PreservedComponents`. `Parent` has no column, because the parent row already encodes it;
- the id generator state `(seed, counter)`, the next creation sequence, and the frame number the caller passed in.

Derived state (`WorldTransform`, render slots, GPU handles) is not captured. Its owners rebuild it from the restored components. `Children` is captured only as order; the lists themselves are rebuilt by attaching.

A column stores values in the component's own EnTT packed order, not in row order, with a parallel `rows` array mapping each value to its entity row. Capture is then a sequential read of each storage. A trivially copyable component is taken with one `memcpy` per storage page:

{{cite ohao/ecs/snapshot_column.hpp "std::memcpy(values->data() + first, pages[first / page], std::min(page, n - first) * sizeof(T));"}}

Other types, such as `Name` with its `std::string`, are copied value by value. Rows come from the `EntityId` storage. If the `CreationOrder` storage is aligned with it, each sequence is read by position rather than looked up. If the sequences come out ascending, as they do in a world built by `World::create`, no sort is needed; otherwise the rows are stable-sorted.

Recapturing into an existing snapshot reuses its buffers. A column whose type is unchanged keeps its value vector:

{{cite ohao/ecs/snapshot.cpp "if (c.type != t) c = SnapshotColumn{t, {}, {}, 0};"}}

The working vectors are `thread_local`, so once the world stops growing, no vector reallocates on a recapture. The header claims that "recapturing allocates nothing". That is not strictly true for `Name`: the column is cleared and copy-constructed again, so any name longer than the small-string buffer allocates on every capture. `world.snapshot` with a name it already holds recaptures in place. `t10_recapture_reuses_memory_and_matches_a_fresh_snapshot` checks that the `ids` buffer pointer is unchanged. `Snapshot` is move-only because of this reuse.

## Restore is a diff

`restoreSnapshot` works through six steps, and every write goes through the registry.

**1. Match.** Row `r` is first tried against position `r` of the `EntityId` storage, because an unchanged world still holds it there. Only when that fails does it fall back to the id index. Each matched entity is stamped. If its `CreationOrder` differs from the saved one, the sequence is corrected and counted as a change.

**2. Destroy extras.** This runs only when the world holds more managed entities than were matched:

{{cite ohao/ecs/snapshot.cpp "if (!intoEmpty && world.idCount() > n - missing) {"}}

Extras are destroyed with `registry().destroy`, not `World::destroy`. A snapshot entity that was reparented under an extra is therefore detached by the hierarchy hook rather than destroyed with it, and step 5 reattaches it.

**3. Create the missing rows**, in row order, through `createWithId` with the saved id, `Name` and `Transform`, and then the saved sequence:

{{cite ohao/ecs/snapshot.cpp "reg.get<CreationOrder>(e).seq = s.order[r];"}}

Recreated entities get back their original `CreationOrder`, so slot order in the render extract and light order in the light table, both sorted by it, come back exactly. `t10_restore_after_structural_changes_keeps_creation_order` destroys two of eight lights, adds an intruder, restores, and requires the light order to be identical.

**4. Columns.** For each value, the entity's current component is compared field by field, bit-exactly (so `-0` is not `+0`), with the same `equalStruct` the save uses for default-omission:

{{cite ohao/ecs/snapshot_column.hpp "if (reflect::equalStruct(cur, &values[k], *col.type)) continue;"}}

Only a value that differs is replaced, and only a missing one is emplaced. A managed entity holding a component the column does not list loses it:

{{cite ohao/ecs/snapshot_column.hpp "if (sc.stamp[entityIndex(e)] != epoch && r.all_of<EntityId>(e)) drop.push_back(e);"}}

The current value is found by the same positional trick: index `k` of the storage, if it still holds that entity, before any sparse lookup.

**5. Hierarchy.** Every row whose current parent differs from the saved one is detached first. Then all of them are attached with the trusted `World::attach`:

{{cite ohao/ecs/snapshot.cpp "if (current[r] != entt::null) world.setParent(live[r], entt::null);"}}

Detaching everything before attaching anything means no intermediate state can contain a cycle, even when two entities swap roles as parent and child. Every attached link is a snapshot link, so the ancestor walk is unnecessary. Each `Children` list is then compared with the saved order and overwritten only if it differs. The check is skipped when the restore attached every link itself and every saved list is in ascending row order.

**6.** The id generator and the creation sequence are set back to the snapshot's values. Entities created after a restore get the same ids and sequences they got the first time (`t10_id_counter_and_creation_sequence_are_restored`).

`RestoreStats` reports `created`, `destroyed` and `changed`. `changed` counts writes, removals, reparents and sequence corrections on entities that existed before the restore; a recreated entity's own writes are not counted. `world.restore` also sets the session's frame back to the snapshot's.

:::key
An unchanged restore performs no emplace, replace, remove or reparent, so it fires no component signal. `t10_unchanged_restore_writes_no_component` connects counters to construct, update and destroy on `EntityId`, `Name`, `Transform`, `Parent`, `Light` and `PreservedComponents`, and requires zero. After one `Transform` edit it requires exactly one write.
:::

Restore does not need an empty world. `t10_mutations_then_restore_give_identical_saves` runs moves, adds, removes, renames, reparents, subtree destroys, raw destroys and creates, then restores, and requires a byte-identical save.

## Restoring into an empty world

The diff above pays for its generality with passes over the whole world: match every row, stamp, compare every column, sweep every pool for values to drop, walk the hierarchy again to attach. At 10,000 entities the world and the snapshot stay in the CPU caches and the extra passes are nearly free. At 100,000 each one streams megabytes from memory, and a parent's `Children` list, allocated in step 3, has long been evicted by the time step 5 appends to it. Restoring into a world with no entity at all needs none of that, and is detected up front:

{{cite ohao/ecs/snapshot.cpp "const bool intoEmpty = world.idCount() == 0 && reg.storage<entt::entity>().in_use() == 0;"}}

The writes are the same; the passes are fewer.

- **Steps 1 and 2** are skipped: every row is missing and nothing is extra.
- **Step 3** attaches each link as it creates the child, while the parent (created a few rows earlier in a typical scene) is still in cache. Children then fill in row order, which is the snapshot's order when its child lists ascend. It does so only when every parent row precedes its child's and the lists ascend; otherwise a separate pass attaches the links from the snapshot's own lists. Either way the `Parent` pool is reserved once.

{{cite ohao/ecs/snapshot.cpp "for (std::size_t r = 0; attachHere && r < n; ++r) attachHere = s.parent[r] == kNoRow || s.parent[r] < r;"}}

- **Step 4** skips the `Name` and `Transform` columns, which step 3 has just written from the same values, and emplaces every other column in one sequential pass, with no lookup, comparison or drop sweep. Each pool is first reserved to its column's size, so it fills once instead of reallocating as it doubles. A pool that already holds the values is left alone:

{{cite ohao/ecs/snapshot_column.hpp "if (const uint32_t row = col.rows[k]; row != kNoRow) r.emplace<T>(sc.live[row], values[k]);"}}

`t10_restore_into_an_empty_world` requires a byte-identical save, the same `Children` order and the same `CreationOrder` per entity, and `scene_bench` checks the bench scenes the same way after timing them. The change took restore into an empty world from 25.6 to 21.5 ms at 100k entities and from 2.40 to 2.06 ms at 10k, measured back to back with `scene_bench --breakdown`.

## How the GPU follows

Restore has no renderer hook, and it needs none. A replaced `Transform` fires `on_update<Transform>`, which the `World` turns into a `TransformDirty` tag:

{{cite ohao/ecs/world.cpp "m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);"}}

The transform system recomputes the world matrix and tags `RenderDirty`, and the render extract updates that one instance in the `RenderScene`. `Light` writes mark the light list dirty, and `MeshSource` and `MaterialOverride` writes mark the entity for instantiate. Because an unchanged restore fires nothing, nothing is dirtied, and the frame after it uploads no instance bytes.

`restore_render_gpu_test` checks this on the checked-in Cornell scene through the CLI's own `Session` and `RenderHost`, deferred at 960x540 with validation on. It moves the metal sphere, renders, then restores. The restore's frame must upload exactly the instance bytes the move's frame uploaded, which is one instance back rather than the whole table:

{{cite tests/renderer/restore_render_gpu_test.cpp "q.restoreBytes == q.moveBytes"}}

A second restore must report `{created: 0, destroyed: 0, changed: 0}` and upload 0 instance bytes. With post effects off, the capture after the restore must be bit-identical to the first.

The pixels need one more thing. The deferred path carries temporal state across frames: TAA history, RT GI history, a clock and the frame slot that indexes TAA's jitter. A capture taken after restoring would otherwise blend in history from the moved sphere. `RenderHost` resets it before every capture except the first, so that the first capture still renders exactly as a fresh renderer does and the goldens hold:

{{cite tools/ohao/render_host.cpp "if (m_captures > 0) renderer.resetAccumulation();"}}

In deferred mode, `VulkanRenderer::resetAccumulation` idles the device, zeroes the frame slot and calls `DeferredRenderer::resetTemporalState`. In the RT modes it resets the path tracer's accumulation. With TAA, SSAO and bloom on, the test accepts a 1 LSB maximum difference: its own comment records that two fresh deferred renderers already differ by that much under those effects.

## Sharp edges

- **The reset belongs to the capture, not the restore.** A windowed host that restores mid-run keeps its TAA and RT GI history unless it calls `resetAccumulation` itself.
- **Silent in-place writes.** The `CreationOrder` correction in step 1 and the `Children` reordering in step 5 write into the component in place, with no signal. A system that ordered work by the old sequence is not told.
- **Only `Parent` is remapped.** Columns copy values verbatim. `Parent` is today the only saved component holding an `entt::entity`, and it is stored as a row. A future saved component with an entity field would be restored with the old handle, which is stale if restore recreated its target.
- **The registry must outlive the snapshot.** Columns hold `const TypeInfo*`, and restore dispatches through `componentOps(*c.type)`. The snapshot must come from a world with the same registrations, and nothing checks that.
- **Unmanaged entities are invisible.** An entity without an `EntityId` is neither captured nor destroyed, and its components are never touched.
- **Snapshots live in memory only**, in the session, by name.
- **Restoring into an empty world is a page-fault workload.** About a quarter of its time at 100k entities is first touches of fresh pages (roughly 6,500 faults). A 10k world often lands in memory the heap kept from an earlier free and takes far fewer, which is why `scene_bench` measures its scaling ratio in blocks that give both sizes the same memory history (spec, "Budgets").
