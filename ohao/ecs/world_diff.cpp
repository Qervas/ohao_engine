#include "ecs/world_diff.hpp"

#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"

#include <algorithm>

namespace ohao::ecs {

namespace {

using detail::entityIndex;

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

void diffWorld(const World& world, const Snapshot& s, WorldDiff& out) {
    out.changed.clear();
    out.created.clear();
    out.destroyed.clear();
    const entt::registry& reg = world.registry();
    const std::size_t n = s.rows();

    // 1. The live entity of each row. Fast path as in restoreSnapshot: an unchanged world
    // holds row r at packed position r.
    thread_local std::vector<entt::entity> live;
    thread_local std::vector<uint32_t> rowOfLive;
    live.assign(n, entt::entity{entt::null});
    rowOfLive.assign(entitySlots(reg), kNoRow);
    const auto* ids = reg.storage<EntityId>();
    for (std::size_t r = 0; r < n; ++r) {
        entt::entity e = entt::null;
        if (ids && r < ids->size() && at<EntityId>(*ids, r).value == s.ids[r]) e = ids->data()[r];
        else e = world.findById(s.ids[r]);
        live[r] = e;
        if (e == entt::null) out.destroyed.push_back(s.ids[r]);
        else rowOfLive[entityIndex(e)] = static_cast<uint32_t>(r);
    }

    // 2. Entities the snapshot does not have, in creation order.
    if (ids && ids->size() > n - out.destroyed.size()) {
        std::vector<std::pair<uint64_t, uint64_t>> fresh;   // (creation seq, id)
        for (std::size_t i = 0; i < ids->size(); ++i) {
            const entt::entity e = ids->data()[i];
            if (rowOfLive[entityIndex(e)] != kNoRow) continue;
            const auto* co = reg.try_get<CreationOrder>(e);
            fresh.emplace_back(co ? co->seq : UINT64_MAX, at<EntityId>(*ids, i).value);
        }
        std::sort(fresh.begin(), fresh.end());
        for (const auto& [seq, id] : fresh) out.created.push_back(id);
    }

    // 3. Component values, column by column; then the hierarchy.
    thread_local std::vector<ColumnChange> changes;
    thread_local std::vector<uint32_t> columnOf;   // per change: its column
    thread_local std::vector<std::pair<uint32_t, uint32_t>> order;   // (row, index into changes)
    changes.clear();
    columnOf.clear();
    DiffScratch scratch{live, rowOfLive, {}, 0};
    scratch.stamp.assign(rowOfLive.size(), 0);
    for (uint32_t c = 0; c < s.columns.size(); ++c) {
        componentOps(*s.columns[c].type)->diff(reg, s.columns[c], scratch, changes);
        columnOf.resize(changes.size(), c);
    }
    thread_local std::vector<uint8_t> parentMoved;
    parentMoved.assign(n, 0);
    std::size_t moved = 0;
    for (std::size_t r = 0; r < n; ++r) {
        if (live[r] == entt::null) continue;
        const auto* p = reg.try_get<Parent>(live[r]);
        const uint64_t now = p ? world.idOf(p->entity) : 0;
        const uint64_t then = s.parent[r] == kNoRow ? 0 : s.ids[s.parent[r]];
        if (now != then) {
            parentMoved[r] = 1;
            ++moved;
        }
    }
    if (changes.empty() && moved == 0) return;

    // 4. Group by row (creation order); a stable sort keeps each entity's components in
    // column order, since the columns were diffed in order.
    order.clear();
    for (uint32_t i = 0; i < changes.size(); ++i) order.emplace_back(changes[i].row, i);
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::size_t k = 0;
    for (uint32_t r = 0; r < n; ++r) {
        const bool hasValues = k < order.size() && order[k].first == r;
        if (!hasValues && !parentMoved[r]) continue;
        EntityDiff d;
        d.id = s.ids[r];
        d.entity = live[r];
        for (; k < order.size() && order[k].first == r; ++k) {
            const ColumnChange& c = changes[order[k].second];
            d.components.push_back({s.columns[columnOf[order[k].second]].type, c.before, c.after});
        }
        if (parentMoved[r]) {
            d.parentChanged = true;
            d.parentBefore = s.parent[r] == kNoRow ? 0 : s.ids[s.parent[r]];
            const auto* p = reg.try_get<Parent>(live[r]);
            d.parentAfter = p ? world.idOf(p->entity) : 0;
        }
        out.changed.push_back(std::move(d));
    }
}

} // namespace ohao::ecs
