#include "gpu/vulkan/barriers.hpp"

namespace ohao::gpu {

namespace {

// The barrier between what last touched a resource and its next use, or nothing when none is needed; `s` becomes
// the state after the use. A read joins the reads since the last write; a write waits for all of them.
struct Need {
    bool barrier{false};
    VkPipelineStageFlags2 srcStage{0};
    VkAccessFlags2 srcAccess{0};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED};
};

Need advance(State& s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write,
             bool discard) {
    Need n{};
    n.srcStage = s.writeStage | s.readStages;
    n.srcAccess = s.writeAccess;
    n.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : s.layout;
    if (write || s.layout != layout) {
        n.barrier = n.srcStage != 0 || s.layout != layout || discard;
        if (write) {
            s = State{layout, stage, access, 0, 0};
        } else {   // a change of layout for a read: the readers so far were waited for; the write stays the write
            s.layout = layout;
            s.readStages = stage;
            s.readAccess = access;
        }
        return n;
    }
    if ((stage & ~s.readStages) != 0 || (access & ~s.readAccess) != 0) {   // a reader the write is not visible to yet
        n.barrier = s.writeStage != 0;
        n.srcStage = s.writeStage;
        s.readStages |= stage;
        s.readAccess |= access;
    }
    return n;
}

} // namespace

void Barriers::image(VkImage image, VkImageAspectFlags aspect, State& s, uint32_t mip, uint32_t layer,
                     VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write,
                     bool discard) {
    const Need n = advance(s, layout, stage, access, write, discard);
    if (!n.barrier) return;
    if (n.oldLayout == layout) {   // no layout change: a memory dependency is all it needs
        memory(n.srcStage, n.srcAccess, stage, access);
        return;
    }
    // One barrier for a run of mips of a layer that need the same thing (the common case: all of them), or for the
    // next layer of the same single mip.
    if (!m_images.empty()) {
        VkImageMemoryBarrier2& last = m_images.back();
        auto& r = last.subresourceRange;
        const bool same = last.image == image && last.srcStageMask == n.srcStage && last.srcAccessMask == n.srcAccess &&
                          last.oldLayout == n.oldLayout && last.newLayout == layout && last.dstStageMask == stage &&
                          last.dstAccessMask == access;
        if (same && r.baseArrayLayer == layer && r.layerCount == 1 && r.baseMipLevel + r.levelCount == mip) {
            ++r.levelCount;
            return;
        }
        if (same && r.baseMipLevel == mip && r.levelCount == 1 && r.baseArrayLayer + r.layerCount == layer) {
            ++r.layerCount;
            return;
        }
    }
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = n.srcStage;
    b.srcAccessMask = n.srcAccess;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.oldLayout = n.oldLayout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, mip, 1, layer, 1};
    m_images.push_back(b);
}

void Barriers::buffer(State& s, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write) {
    const Need n = advance(s, VK_IMAGE_LAYOUT_UNDEFINED, stage, access, write, false);
    if (n.barrier && n.srcStage != 0) memory(n.srcStage, n.srcAccess, stage, access);
}

void Barriers::memory(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
    m_memory.srcStageMask |= srcStage;
    m_memory.srcAccessMask |= srcAccess;
    m_memory.dstStageMask |= dstStage;
    m_memory.dstAccessMask |= dstAccess;
}

void Barriers::record(VkCommandBuffer cmd) {
    if (empty()) return;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = m_memory.dstStageMask != 0 ? 1u : 0u;
    dep.pMemoryBarriers = &m_memory;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(m_images.size());
    dep.pImageMemoryBarriers = m_images.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    m_images.clear();
    m_memory = VkMemoryBarrier2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
}

} // namespace ohao::gpu
