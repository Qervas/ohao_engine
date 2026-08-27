#pragma once

#include <cstddef>
#include <vector>

namespace ohao::diff {

struct ArenaBlock {
    std::size_t offsetBytes{0};
    std::size_t sizeBytes{0};
};

/// Suballocation arithmetic for the gradient arena. Pure -- no Vulkan.
///
/// Every gradient and optimizer-state allocation lives in one VkBuffer so that
/// per-iteration zeroing is a single vkCmdFillBuffer rather than N clears.
class ArenaLayout {
public:
    static constexpr std::size_t kAlignmentBytes = 256;
    static constexpr std::size_t kInvalidBlock = static_cast<std::size_t>(-1);

    /// Reserve `floatCount` float32s. Returns the block index, or kInvalidBlock
    /// if floatCount is zero.
    std::size_t add(std::size_t floatCount);

    [[nodiscard]] ArenaBlock block(std::size_t index) const;
    [[nodiscard]] std::size_t blockCount() const noexcept { return m_blocks.size(); }
    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_cursorBytes; }

private:
    std::vector<ArenaBlock> m_blocks;
    std::size_t m_cursorBytes{0};
};

}  // namespace ohao::diff
