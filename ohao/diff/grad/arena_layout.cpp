#include "diff/grad/arena_layout.hpp"

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
    // NO ASSERT HERE, deliberately, and NOT the "assert plus unconditional
    // guard" pattern WavefrontLoop::record and WavefrontStage::record use.
    //
    // Those two assert on PROGRAMMER ERROR -- recording a stage that was
    // never built -- which no caller is entitled to do and no test performs.
    // An out-of-range index here is different: returning the invalid block
    // (sizeBytes == 0) is a DOCUMENTED PART OF THIS API, callers are told to
    // test sizeBytes before reading offsetBytes, and
    // DiffArenaLayout.OutOfRangeIndexReturnsInvalidBlock exercises exactly
    // that path.
    //
    // With an assert in front of it, that path aborted the whole test binary
    // in any build where asserts are live. It went unnoticed because the
    // Release test targets define NDEBUG, so the assert compiled away and
    // the graceful return was all anyone ever ran -- a test that passed only
    // because the shipping configuration hid the code it was testing.
    if (index >= m_blocks.size()) return ArenaBlock{};
    return m_blocks[index];
}

}  // namespace ohao::diff
