#include "diff/grad/arena_layout.hpp"

#include <cassert>

namespace ohao::diff {
namespace {

std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace

std::size_t ArenaLayout::add(std::size_t floatCount) {
    if (floatCount == 0) return kInvalidBlock;

    ArenaBlock b;
    b.offsetBytes = m_cursorBytes;
    b.sizeBytes = floatCount * sizeof(float);

    m_cursorBytes = alignUp(b.offsetBytes + b.sizeBytes, kAlignmentBytes);
    m_blocks.push_back(b);
    return m_blocks.size() - 1;
}

ArenaBlock ArenaLayout::block(std::size_t index) const {
    assert(index < m_blocks.size() && "ArenaLayout::block: index out of range");
    if (index >= m_blocks.size()) return ArenaBlock{};
    return m_blocks[index];
}

}  // namespace ohao::diff
