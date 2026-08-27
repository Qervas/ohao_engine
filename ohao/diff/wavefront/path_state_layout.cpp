#include "diff/wavefront/path_state_layout.hpp"

namespace ohao::diff {

PathStateLayout::PathStateLayout(std::uint32_t capacity) {
    for (std::uint32_t i = 0; i < kFieldCount; ++i) {
        m_blocks[i] = ArenaLayout::kInvalidBlock;
    }
    if (capacity == 0) return;

    m_capacity = capacity;
    for (std::uint32_t i = 0; i < kFieldCount; ++i) {
        m_blocks[i] = m_arena.add(capacity);
    }
}

std::size_t PathStateLayout::block(PathStateField field) const {
    const auto i = static_cast<std::uint32_t>(field);
    if (i >= kFieldCount) return ArenaLayout::kInvalidBlock;
    return m_blocks[i];
}

}  // namespace ohao::diff
