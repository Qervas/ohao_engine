#include "ecs/snapshot.hpp"

#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"

#include <algorithm>
#include <cstring>

namespace ohao::ecs {

namespace {

using detail::entityIndex;

// Entity slots ever handed out: every live entity's index is below it.
std::size_t entitySlots(const entt::registry& r) {
    const auto* s = r.storage<entt::entity>();
    return s ? s->size() : 0;
}

// Element i of a packed storage, without a sparse lookup.
template <class T, class S>
const T& at(const S& s, std::size_t i) {
    constexpr std::size_t page = entt::component_traits<T>::page_size;
    return s.raw()[i / page][i % page];
}

} // namespace

std::size_t Snapshot::bytes() const noexcept {
    std::size_t n = ids.size() * 8 + order.size() * 8 + parent.size() * 4 + childBegin.size() * 4 + children.size() * 4;
    for (const SnapshotColumn& c : columns) n += c.rows.size() * 4 + c.valueBytes;
    return n;
}

// Everything is read in EnTT packed order, which for a world built by World::create is
// creation order: the reads are sequential, with no sparse or hash lookups per entity.
Snapshot takeSnapshot(const World& world, const reflect::TypeRegistry& types, uint64_t frame) {
    Snapshot s;
    takeSnapshot(world, types, s, frame);
    return s;
}

void takeSnapshot(const World& world, const reflect::TypeRegistry& types, Snapshot& s, uint64_t frame) {
    const entt::registry& reg = world.registry();
    const auto* idStore = reg.storage<EntityId>();
    const std::size_t n = idStore ? idStore->size() : 0;
    const entt::entity* es = n ? idStore->data() : nullptr;

    // CreationOrder per packed position. Both storages gain and lose entities together,
    // so they are normally aligned; otherwise look each one up.
    const auto* coStore = reg.storage<CreationOrder>();
    const bool aligned = coStore && coStore->size() == n && (n == 0 || std::memcmp(coStore->data(), es, n * sizeof(entt::entity)) == 0);
    thread_local std::vector<uint64_t> seq;   // working memory, kept warm between snapshots
    thread_local std::vector<uint32_t> pos;
    thread_local std::vector<uint32_t> rowOf;
    seq.resize(n);
    bool sorted = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (aligned) seq[i] = at<CreationOrder>(*coStore, i).seq;
        else if (const auto* co = reg.try_get<CreationOrder>(es[i])) seq[i] = co->seq;
        else seq[i] = UINT64_MAX;
        sorted = sorted && (i == 0 || seq[i - 1] <= seq[i]);
    }
    // Rows in creation order: packed position of each row.
    pos.resize(n);
    for (uint32_t i = 0; i < n; ++i) pos[i] = i;
    if (!sorted) std::stable_sort(pos.begin(), pos.end(), [&](uint32_t a, uint32_t b) { return seq[a] < seq[b]; });

    rowOf.assign(entitySlots(reg), kNoRow);
    s.ids.resize(n);
    s.order.resize(n);
    for (std::size_t r = 0; r < n; ++r) {
        const uint32_t i = pos[r];
        rowOf[entityIndex(es[i])] = static_cast<uint32_t>(r);
        s.ids[r] = at<EntityId>(*idStore, i).value;
        s.order[r] = seq[i];
    }
    const auto rowOfEntity = [&](entt::entity e) {
        const uint32_t idx = entityIndex(e);
        return e != entt::null && idx < rowOf.size() ? rowOf[idx] : kNoRow;
    };

    // Hierarchy: parent rows, then child rows in Children order (CSR).
    s.parent.assign(n, kNoRow);
    if (const auto* ps = reg.storage<Parent>())
        for (std::size_t i = 0; i < ps->size(); ++i)
            if (const uint32_t r = rowOfEntity(ps->data()[i]); r != kNoRow) s.parent[r] = rowOfEntity(at<Parent>(*ps, i).entity);
    s.childBegin.assign(n + 1, 0);
    s.children.clear();
    const auto* cs = reg.storage<Children>();
    if (cs && sorted && cs->size() == n && std::memcmp(cs->data(), es, n * sizeof(entt::entity)) == 0) {
        // Aligned with the rows: one pass, appending row by row.
        for (std::size_t r = 0; r < n; ++r) {
            s.childBegin[r] = static_cast<uint32_t>(s.children.size());
            for (const entt::entity c : at<Children>(*cs, r).entities)
                if (const uint32_t cr = rowOfEntity(c); cr != kNoRow) s.children.push_back(cr);
        }
        s.childBegin[n] = static_cast<uint32_t>(s.children.size());
    } else if (cs) {
        for (std::size_t i = 0; i < cs->size(); ++i)
            if (const uint32_t r = rowOfEntity(cs->data()[i]); r != kNoRow)
                for (const entt::entity c : at<Children>(*cs, i).entities) s.childBegin[r + 1] += rowOfEntity(c) != kNoRow;
        for (std::size_t r = 0; r < n; ++r) s.childBegin[r + 1] += s.childBegin[r];
        s.children.resize(s.childBegin[n]);
        for (std::size_t i = 0; i < cs->size(); ++i)
            if (const uint32_t r = rowOfEntity(cs->data()[i]); r != kNoRow) {
                uint32_t k = s.childBegin[r];
                for (const entt::entity c : at<Children>(*cs, i).entities)
                    if (const uint32_t cr = rowOfEntity(c); cr != kNoRow) s.children[k++] = cr;
            }
    }

    s.childrenAscending = true;
    for (std::size_t r = 0; r < n && s.childrenAscending; ++r)
        for (uint32_t k = s.childBegin[r] + 1; k < s.childBegin[r + 1]; ++k)
            if (s.children[k - 1] > s.children[k]) {
                s.childrenAscending = false;
                break;
            }

    // One column per saved component (Parent is the parent row), then the preserved ones.
    // A column whose type matches the one already in that slot keeps its buffers.
    std::size_t used = 0;
    const auto capture = [&](const reflect::TypeInfo* t) {
        if (used == s.columns.size()) s.columns.emplace_back();
        SnapshotColumn& c = s.columns[used++];
        if (c.type != t) c = SnapshotColumn{t, {}, {}, 0};
        componentOps(*t)->capture(reg, rowOf, c);
    };
    const reflect::TypeInfo* tParent = types.find<Parent>();
    s.nameColumn = s.transformColumn = -1;
    for (const reflect::TypeInfo* t : types.types()) {
        if (t->role != reflect::TypeRole::Component || !t->serialized || t == tParent || !componentOps(*t)) continue;
        if (t == types.find<Name>()) s.nameColumn = static_cast<int>(used);
        if (t == types.find<Transform>()) s.transformColumn = static_cast<int>(used);
        capture(t);
    }
    if (const reflect::TypeInfo* t = types.find<PreservedComponents>()) capture(t);
    s.columns.resize(used);

    s.idGen = world.idGenState();
    s.nextSeq = world.nextCreationSeq();
    s.frame = frame;
}

RestoreStats restoreSnapshot(World& world, const Snapshot& s) {
    entt::registry& reg = world.registry();
    RestoreStats stats;
    const std::size_t n = s.rows();
    // Working memory, kept warm between restores (an agent restores every step).
    thread_local std::vector<entt::entity> live, current, want;
    thread_local std::vector<uint8_t> fresh, checked;
    thread_local std::vector<uint32_t> stamp, moved;
    live.assign(n, entt::entity{entt::null});
    fresh.assign(n, 0);
    // Restoring into a world with no entity at all (a new world, or one cleared): every
    // entity is created here, so nothing needs finding, comparing, dropping or detaching.
    // The paths below for that case do the same writes in fewer passes; at 100k entities
    // each pass streams megabytes from memory (spec, "Scaling").
    const bool intoEmpty = world.idCount() == 0 && reg.storage<entt::entity>().in_use() == 0;

    // 1. Which snapshot entities are alive; stamp them. Fast path: an unchanged world
    // holds row r at packed position r (both are creation order).
    std::size_t missing = intoEmpty ? n : 0;
    if (!intoEmpty) {
        stamp.assign(entitySlots(reg), 0);
        auto& ids = reg.storage<EntityId>();
        auto& cos = reg.storage<CreationOrder>();
        for (std::size_t r = 0; r < n; ++r) {
            entt::entity e = entt::null;
            if (r < ids.size() && at<EntityId>(ids, r).value == s.ids[r]) e = ids.data()[r];
            else e = world.findById(s.ids[r]);
            live[r] = e;
            if (e == entt::null) {
                ++missing;
                continue;
            }
            stamp[entityIndex(e)] = 1;
            CreationOrder& co = r < cos.size() && cos.data()[r] == e
                                    ? const_cast<CreationOrder&>(at<CreationOrder>(cos, r))
                                    : reg.get<CreationOrder>(e);
            if (co.seq != s.order[r]) {
                co.seq = s.order[r];
                ++stats.changed;
            }
        }
    }

    // 2. Destroy entities the snapshot does not have. registry().destroy detaches their
    // children instead of destroying them: those that belong to the snapshot are
    // reattached below.
    if (!intoEmpty && world.idCount() > n - missing) {
        std::vector<entt::entity> extra;
        for (const entt::entity e : static_cast<const entt::sparse_set&>(reg.storage<EntityId>()))
            if (stamp[entityIndex(e)] != 1) extra.push_back(e);
        for (const entt::entity e : extra) reg.destroy(e);
        stats.destroyed = extra.size();
    }

    // 3. Create the missing ones, in creation order, with their id, name, transform and
    // CreationOrder.
    bool attachHere = false;   // the hierarchy was attached while creating (into an empty world)
    if (missing) {
        world.reserve(world.idCount() + missing);
        thread_local std::vector<uint32_t> nameAt, transformAt;
        nameAt.assign(n, kNoRow);
        transformAt.assign(n, kNoRow);
        const SnapshotColumn* nameCol = s.nameColumn >= 0 ? &s.columns[s.nameColumn] : nullptr;
        const SnapshotColumn* transformCol = s.transformColumn >= 0 ? &s.columns[s.transformColumn] : nullptr;
        if (nameCol)
            for (uint32_t k = 0; k < nameCol->rows.size(); ++k)
                if (nameCol->rows[k] != kNoRow) nameAt[nameCol->rows[k]] = k;
        if (transformCol)
            for (uint32_t k = 0; k < transformCol->rows.size(); ++k)
                if (transformCol->rows[k] != kNoRow) transformAt[transformCol->rows[k]] = k;
        const auto* names = nameCol ? static_cast<const std::vector<Name>*>(nameCol->values.get()) : nullptr;
        const auto* transforms =
            transformCol ? static_cast<const std::vector<Transform>*>(transformCol->values.get()) : nullptr;
        // Into an empty world, a link whose parent row comes first is attached right here,
        // while the parent's Children (created a few rows earlier) is still in cache:
        // attached in a later pass, each link is a cache miss once the world outgrows the
        // CPU caches. Children then fill in row order, which is the snapshot's order when
        // its child lists ascend. Otherwise step 5 attaches every link.
        attachHere = intoEmpty && s.childrenAscending;
        for (std::size_t r = 0; attachHere && r < n; ++r) attachHere = s.parent[r] == kNoRow || s.parent[r] < r;
        auto& parents = reg.storage<Parent>();
        auto& childLists = reg.storage<Children>();
        if (attachHere) parents.reserve(n - static_cast<std::size_t>(std::count(s.parent.begin(), s.parent.end(), kNoRow)));
        for (std::size_t r = 0; r < n; ++r) {
            if (live[r] != entt::null) continue;
            const std::string_view name = nameAt[r] != kNoRow ? std::string_view((*names)[nameAt[r]].value) : "";
            const Transform local = transformAt[r] != kNoRow ? (*transforms)[transformAt[r]] : Transform{};
            const entt::entity e = world.createWithId(s.ids[r], name, local);
            reg.get<CreationOrder>(e).seq = s.order[r];
            // One allocation for its children instead of growing per child.
            if (const uint32_t kids = s.childBegin[r + 1] - s.childBegin[r]) childLists.get(e).entities.reserve(kids);
            if (attachHere && s.parent[r] != kNoRow) {
                const entt::entity p = live[s.parent[r]];
                reg.emplace<Parent>(e, p);
                childLists.get(p).entities.push_back(e);
            }
            live[r] = e;
            fresh[r] = 1;
        }
        stats.created = missing;
    }

    // 4. Component values, column by column. Into an empty world, the Name and Transform
    // columns are exactly what step 3 wrote (comparing them again would only take a cache
    // miss per entity), and the other columns are plain emplaces.
    if (!intoEmpty) stamp.assign(entitySlots(reg), 0);
    RestoreScratch scratch{live, fresh, std::move(stamp), 0, intoEmpty};
    for (std::size_t i = 0; i < s.columns.size(); ++i) {
        const SnapshotColumn& c = s.columns[i];
        if (intoEmpty && (static_cast<int>(i) == s.nameColumn || static_cast<int>(i) == s.transformColumn)) continue;
        stats.changed += componentOps(*c.type)->restore(reg, c, scratch);
    }
    stamp = std::move(scratch.stamp);

    // 5 (into an empty world). No entity has a parent yet: each link is one Parent emplace,
    // and each Children list is filled from the snapshot's, so it is in the snapshot's
    // order already (step 3 reserved it, and created every entity with TransformDirty).
    if (intoEmpty) {
        if (!attachHere) {
            std::size_t links = 0;
            for (std::size_t r = 0; r < n; ++r) links += s.parent[r] != kNoRow;
            reg.storage<Parent>().reserve(links);
            for (std::size_t r = 0; r < n; ++r)
                if (s.parent[r] != kNoRow) reg.emplace<Parent>(live[r], live[s.parent[r]]);
            auto& cs = reg.storage<Children>();
            for (std::size_t r = 0; r < n; ++r)
                if (s.childBegin[r] != s.childBegin[r + 1]) {
                    auto& kids = cs.get(live[r]).entities;
                    for (uint32_t k = s.childBegin[r]; k < s.childBegin[r + 1]; ++k) kids.push_back(live[s.children[k]]);
                }
        }
        world.setIdGenState(s.idGen);
        world.setNextCreationSeq(s.nextSeq);
        return stats;
    }

    // 5. Hierarchy. Current parents come from the Parent storage in packed order. Detach
    // every entity whose parent differs first, then attach: the attached links are all
    // snapshot links, so no intermediate state has a cycle.
    std::vector<uint32_t>& rowOfLive = stamp;   // reused: entity index -> row
    std::fill(rowOfLive.begin(), rowOfLive.end(), kNoRow);
    for (std::size_t r = 0; r < n; ++r) rowOfLive[entityIndex(live[r])] = static_cast<uint32_t>(r);
    const auto rowOfEntity = [&](entt::entity e) {
        const uint32_t idx = entityIndex(e);
        return e != entt::null && idx < rowOfLive.size() ? rowOfLive[idx] : kNoRow;
    };
    current.assign(n, entt::entity{entt::null});
    {
        const auto& ps = reg.storage<Parent>();
        for (std::size_t i = 0; i < ps.size(); ++i)
            if (const uint32_t r = rowOfEntity(ps.data()[i]); r != kNoRow) current[r] = at<Parent>(ps, i).entity;
    }
    const auto desiredParent = [&](std::size_t r) {
        return s.parent[r] == kNoRow ? entt::entity{entt::null} : live[s.parent[r]];
    };
    moved.clear();
    std::size_t parented = 0;
    for (std::size_t r = 0; r < n; ++r) {
        parented += s.parent[r] != kNoRow;
        if (current[r] == desiredParent(r)) continue;
        moved.push_back(static_cast<uint32_t>(r));
        if (current[r] != entt::null) world.setParent(live[r], entt::null);
    }
    // Every link attached is a snapshot link and every entity attached is detached by now,
    // so the trusted attach applies (no ancestor walk per entity). The Parent pool takes
    // every link at once instead of growing by doubling.
    reg.storage<Parent>().reserve(parented);
    for (const uint32_t r : moved) {
        if (const entt::entity p = desiredParent(r); p != entt::null) world.attach(live[r], p);
        stats.changed += fresh[r] ? 0u : 1u;
    }

    // Children order: the sets now match the snapshot; put them in its order. When this
    // restore attached every link itself (an empty or fully rebuilt world), in row order,
    // each Children list is already in row order: nothing to check if the snapshot's are.
    const std::size_t attached = static_cast<std::size_t>(
        std::count_if(moved.begin(), moved.end(), [&](uint32_t r) { return s.parent[r] != kNoRow; }));
    if (attached == parented && s.childrenAscending) {
        world.setIdGenState(s.idGen);
        world.setNextCreationSeq(s.nextSeq);
        return stats;
    }
    checked.assign(n, 0);
    const auto matchOrder = [&](uint32_t r, std::vector<entt::entity>& have) {
        checked[r] = 1;
        const uint32_t b = s.childBegin[r], e = s.childBegin[r + 1];
        bool same = have.size() == e - b;
        for (uint32_t k = b; same && k < e; ++k) same = have[k - b] == live[s.children[k]];
        if (same) return;
        want.clear();
        for (uint32_t k = b; k < e; ++k) want.push_back(live[s.children[k]]);
        have = want;
    };
    {
        auto& cs = reg.storage<Children>();
        for (std::size_t i = 0; i < cs.size(); ++i)
            if (const uint32_t r = rowOfEntity(cs.data()[i]); r != kNoRow)
                matchOrder(r, const_cast<Children&>(at<Children>(cs, i)).entities);
    }
    for (uint32_t r = 0; r < n; ++r)
        if (!checked[r] && s.childBegin[r] != s.childBegin[r + 1]) matchOrder(r, reg.emplace<Children>(live[r]).entities);

    world.setIdGenState(s.idGen);
    world.setNextCreationSeq(s.nextSeq);
    return stats;
}

} // namespace ohao::ecs
