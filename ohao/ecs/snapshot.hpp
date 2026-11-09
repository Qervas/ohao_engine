#pragma once
// In-memory columnar snapshots: the agent's hot loop (snapshot, try something, restore)
// at memcpy speed, with no text involved.
//
// A snapshot captures the authoring state of every entity that has an EntityId:
//   - the entity table, in creation order: id, CreationOrder, parent row, child rows
//     (so Children order comes back exactly);
//   - one column per saved component (Parent excepted: it is the parent row), plus the
//     preserved unknown components, plus BodyMotion: runtime, but physics state the saved
//     components do not hold (velocities, sleep), so a restore mid-motion replays exactly
//     (world.hash and world.diff leave it out: they are about the saved state);
//   - the id generator, the next creation sequence and the caller's frame counter;
//   - what the World's snapshot hooks keep (ecs/snapshot_hook.hpp: physics, the audio schedule, vfx).
// Derived runtime components (Children, WorldTransform, render and GPU state) are not
// captured; their owners rebuild them from the restored components, driven by the same
// signals as any edit.
//
// Restore diffs instead of rebuilding: extra entities are destroyed, missing ones created
// with their captured id and CreationOrder, and only values that differ are written
// (through emplace/replace/remove, so systems see exactly the real changes). Restoring an
// unchanged world writes nothing.
#include "ecs/snapshot_column.hpp"
#include "ecs/world.hpp"
#include "reflect/registry.hpp"

#include "ecs/snapshot_hook.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ohao::ecs {

struct Snapshot {
    Snapshot() = default;
    Snapshot(Snapshot&&) noexcept = default;
    Snapshot& operator=(Snapshot&&) noexcept = default;
    Snapshot(const Snapshot&) = delete;   // columns own buffers that a recapture reuses
    Snapshot& operator=(const Snapshot&) = delete;

    std::vector<uint64_t> ids;         // row -> EntityId; rows are in creation order
    std::vector<uint64_t> order;       // row -> CreationOrder
    std::vector<uint32_t> parent;      // row -> parent row, or kNoRow
    std::vector<uint32_t> childBegin;  // children of row r: children[childBegin[r] .. childBegin[r + 1])
    std::vector<uint32_t> children;    // child rows, in Children order
    bool childrenAscending{true};      // every child list is in row order (restore can skip re-sorting)
    std::vector<SnapshotColumn> columns;
    int nameColumn{-1};        // the Name and Transform columns (entities are created with them)
    int transformColumn{-1};
    int motionColumn{-1};      // BodyMotion's: restored, never diffed
    World::IdGenState idGen{};
    uint64_t nextSeq{0};
    uint64_t frame{0};
    std::shared_ptr<const void> systems;   // the snapshot hooks' states (SystemStates), or null (not counted in bytes())

    [[nodiscard]] std::size_t rows() const noexcept { return ids.size(); }
    [[nodiscard]] std::size_t bytes() const noexcept;   // memory held by the snapshot
};

struct RestoreStats {
    std::size_t created{0};     // entities recreated
    std::size_t destroyed{0};   // entities that did not exist in the snapshot
    std::size_t changed{0};     // component writes and reparents on entities that survived
};

[[nodiscard]] Snapshot takeSnapshot(const World& world, const reflect::TypeRegistry& types, uint64_t frame = 0);
// Captures over an existing snapshot, reusing its memory: recapturing allocates nothing
// once the world has stopped growing (an agent re-snapshotting "base" every step).
void takeSnapshot(const World& world, const reflect::TypeRegistry& types, Snapshot& into, uint64_t frame = 0);
// `snapshot` must come from a world with the same registrations. Never fails. The components
// first, then the snapshot hooks' states.
RestoreStats restoreSnapshot(World& world, const Snapshot& snapshot);

// Puts the World's RoundHook (ecs/round_hook.hpp) in its context: a round's start is a snapshot of
// the world taken with these types, a restart restores it. The world and types must outlive it.
void installRoundHook(World& world, const reflect::TypeRegistry& types);

// The parts of a capture, for code that reads the world in snapshot order without copying
// its values (world.hash): the entity table (ids, creation order, hierarchy, id generator;
// no columns, the frame untouched) with `rowOf` mapping an entity index to its row
// (kNoRow: none), and the component types a snapshot has a column for, in column order
// (`runtime` false: without BodyMotion, the saved state alone).
void captureEntityTable(const World& world, Snapshot& into, std::vector<uint32_t>& rowOf);
[[nodiscard]] std::vector<const reflect::TypeInfo*> snapshotTypes(const reflect::TypeRegistry& types, bool runtime = true);

} // namespace ohao::ecs
