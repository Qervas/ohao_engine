#pragma once
// One component type's column in a Snapshot (ecs/snapshot.hpp), and the typed capture
// and restore code that makeComponentOps<T> instantiates for it. Columns hold values in
// the component's EnTT packed order; trivially copyable components are captured with one
// memcpy per storage page.
#include "ecs/components.hpp"
#include "reflect/type_info.hpp"

#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace ohao::ecs {

inline constexpr uint32_t kNoRow = UINT32_MAX;

struct SnapshotColumn {
    const reflect::TypeInfo* type{nullptr};
    std::vector<uint32_t> rows;           // the entity row of each value; kNoRow: an unmanaged entity
    std::shared_ptr<void> values;         // std::vector<T>, parallel to rows (reused by the next capture)
    std::size_t valueBytes{0};
};

// Working state of one restore, shared by its column passes.
struct RestoreScratch {
    std::span<const entt::entity> live;   // row -> entity
    std::span<const uint8_t> fresh;       // row -> created by this restore (its writes are not "changes")
    std::vector<uint32_t> stamp;          // by entity index: the epoch that last listed it
    uint32_t epoch{0};
};

namespace detail {

[[nodiscard]] inline uint32_t entityIndex(entt::entity e) noexcept {
    return static_cast<uint32_t>(entt::to_entity(e));
}

// `out.values` is reused when it already holds a std::vector<T> (out.type unchanged):
// capturing over an existing snapshot allocates nothing.
template <class T>
void captureColumn(const entt::registry& r, std::span<const uint32_t> rowOf, SnapshotColumn& out) {
    static_assert(!entt::component_traits<T>::in_place_delete, "packed storage expected (no tombstones)");
    if (!out.values) out.values = std::make_shared<std::vector<T>>();
    auto* values = static_cast<std::vector<T>*>(out.values.get());
    values->clear();
    out.rows.clear();
    if (const auto* s = r.storage<T>(); s && !s->empty()) {
        const std::size_t n = s->size();
        const entt::entity* es = s->data();
        out.rows.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const uint32_t idx = entityIndex(es[i]);
            out.rows[i] = idx < rowOf.size() ? rowOf[idx] : kNoRow;
        }
        constexpr std::size_t page = entt::component_traits<T>::page_size;
        const auto* pages = s->raw();
        if constexpr (std::is_trivially_copyable_v<T>) {
            values->resize(n);
            for (std::size_t first = 0; first < n; first += page)
                std::memcpy(values->data() + first, pages[first / page], std::min(page, n - first) * sizeof(T));
        } else {
            values->reserve(n);
            for (std::size_t i = 0; i < n; ++i) values->push_back(pages[i / page][i % page]);
        }
    }
    out.valueBytes = values->size() * sizeof(T);
}

// Emplaces or replaces every listed value that the entity lacks or holds differently
// (bit-exact per registered field), then removes T from managed entities (those with an
// EntityId) the column does not list. Every write goes through the registry, so the
// usual signals fire, and only for real changes. Returns the writes made to entities
// that existed before the restore.
template <class T>
std::size_t restoreColumn(entt::registry& r, const SnapshotColumn& col, RestoreScratch& sc) {
    const auto& values = *static_cast<const std::vector<T>*>(col.values.get());
    auto& s = r.storage<T>();
    const uint32_t epoch = ++sc.epoch;
    constexpr std::size_t page = entt::component_traits<T>::page_size;
    std::size_t writes = 0;
    for (std::size_t k = 0; k < col.rows.size(); ++k) {
        const uint32_t row = col.rows[k];
        if (row == kNoRow) continue;
        const entt::entity e = sc.live[row];
        sc.stamp[entityIndex(e)] = epoch;
        // Fast path: the column was captured in packed order, and an unchanged storage
        // still holds the entity at the same position (no sparse lookup, sequential).
        // The pointer is re-read every time: emplace below can move the storage.
        const T* cur = nullptr;
        if (k < s.size() && s.data()[k] == e) cur = &s.raw()[k / page][k % page];
        else if (s.contains(e)) cur = &s.get(e);
        if (cur) {
            if (reflect::equalStruct(cur, &values[k], *col.type)) continue;
            r.replace<T>(e, values[k]);
        } else {
            r.emplace<T>(e, values[k]);
        }
        writes += sc.fresh[row] ? 0u : 1u;
    }
    std::vector<entt::entity> drop;
    for (const entt::entity e : static_cast<const entt::sparse_set&>(s))
        if (sc.stamp[entityIndex(e)] != epoch && r.all_of<EntityId>(e)) drop.push_back(e);
    for (const entt::entity e : drop) r.remove<T>(e);
    return writes + drop.size();
}

} // namespace detail
} // namespace ohao::ecs
