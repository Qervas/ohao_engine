#include "diff/wavefront/wavefront_loop.hpp"

#include <cassert>
#include <utility>

namespace ohao::diff {
namespace {

/// Every barrier here that has to be visible to a compacting dispatch names
/// BOTH bits. `wf_intersect.comp` and `wf_scatter.comp` hand out compaction
/// offsets with `atomicAdd(counters.value[dstCountSlot], 1u)`, and an atomic
/// is a read-modify-WRITE: a destination scope of `SHADER_READ` alone does
/// not order it against the access that produced the value being added to.
/// This is the precedent gpu_probe_context.cpp's scatter probe sets for its
/// own fill->atomicAdd barrier, and the reason it is spelled out as a named
/// constant is that the omission is invisible -- the wrong version compiles,
/// runs, and produces a plausible-looking image.
constexpr VkAccessFlags kShaderReadWrite =
    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

/// Records one VkBufferMemoryBarrier per entry of `targets` (at most three:
/// state, queue, counter), all with the same stage/access scopes and all
/// covering the whole buffer.
///
/// Whole-buffer rather than per-slot ranges on purpose: the counter buffer's
/// slots are only 4 bytes apart and several of them are touched by different
/// accesses within one bounce, so a byte-range-precise barrier set would be
/// far harder to read and review than the ordering it encodes -- and human
/// review is the only check this code gets (see wavefront_loop.hpp).
void recordBufferBarriers(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess, const VkBuffer* targets, uint32_t count) {
    VkBufferMemoryBarrier barriers[3]{};
    assert(count <= 3 && "recordBufferBarriers: at most state/queue/counter");
    if (count > 3) count = 3;
    for (uint32_t i = 0; i < count; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcAccessMask = srcAccess;
        barriers[i].dstAccessMask = dstAccess;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].buffer = targets[i];
        barriers[i].offset = 0;
        barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, count, barriers, 0, nullptr);
}

}  // namespace

WavefrontLoop::Ring WavefrontLoop::finalLiveRing(std::uint32_t /*capacity*/,
                                                 std::uint32_t /*bounces*/) noexcept {
    // Two compacting dispatches per bounce -- intersect, then scatter -- so
    // the (source, destination) pair is swapped an even number of times per
    // bounce and the live list always lands back where generate put it. The
    // parameters are taken (and ignored) so that a loop which one day grows
    // an odd number of compacting stages, or more than two rings, changes
    // only this function and not every caller.
    return Ring{0u, WavefrontBuffers::kCurrentCountSlot};
}

void WavefrontLoop::recordCompactingStage(VkCommandBuffer cmd, const WavefrontBuffers& buffers,
                                          WavefrontStage& stage, const Ring& src,
                                          const Ring& dst) const {
    const VkBuffer counter = buffers.counterBuffer();
    const VkBuffer allBuffers[3] = {buffers.stateBuffer(), buffers.queueBuffer(), counter};
    const VkDeviceSize argsOffset =
        static_cast<VkDeviceSize>(WavefrontBuffers::kIndirectArgsSlot) * sizeof(std::uint32_t);
    const VkDeviceSize dstSlotOffset =
        static_cast<VkDeviceSize>(dst.countSlot) * sizeof(std::uint32_t);

    // (1) COMPUTE -> TRANSFER, before the fill below.
    //
    // This barrier has no precedent anywhere in the codebase, and no current
    // test would catch its absence. Fusing the loop is what creates the
    // hazard: the fill is a TRANSFER-stage write sitting between two compute
    // dispatches in one command buffer, and it overwrites a counter slot the
    // PRECEDING dispatch reads. At bounce 1, intersect's destination slot is
    // slot 1 -- the very slot bounce 0's scatter read as its source count.
    // Nothing orders the fill after that read but this. Every earlier probe
    // got away without it only because vkQueueWaitIdle separated every
    // dispatch, and removing that wait is the entire point of this class.
    //
    // srcAccessMask names SHADER_READ as well as SHADER_WRITE because the
    // hazard being closed is write-after-READ, not write-after-write.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         kShaderReadWrite, VK_ACCESS_TRANSFER_WRITE_BIT, &counter, 1);

    // (2) Zero the destination count slot.
    //
    // The dispatch below hands out compaction offsets with atomicAdd on this
    // slot, so it must be 0 going in -- and under ping-pong it generally is
    // not, because it is a slot some earlier dispatch already counted into
    // (see wavefront_loop.hpp's "Zeroing the destination slot"). Reusing a
    // stale count as the atomicAdd base displaces every compaction offset
    // after the first, which is silent: paths still trace and the counts
    // still look plausible.
    vkCmdFillBuffer(cmd, counter, dstSlotOffset, sizeof(std::uint32_t), 0u);

    // (3) TRANSFER -> COMPUTE, so the zero is visible to the atomicAdd.
    // dstAccessMask must include SHADER_WRITE: an atomicAdd is a
    // read-modify-write, and a read-only destination scope does not order it.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, kShaderReadWrite, &counter, 1);

    // (4) prepare_indirect: counter[src.countSlot] -> the
    // {groupCountX,1,1} VkDispatchIndirectCommand triple at
    // kIndirectArgsSlot, in this same counter buffer. The live count exists
    // only on the GPU timeline; reading it back to size a dispatch on the
    // host would serialise every bounce on a round-trip and defeat the whole
    // wavefront architecture.
    const PrepareIndirectPush prepPush{src.countSlot, WavefrontBuffers::kIndirectArgsSlot};
    m_prepareIndirect->setPushConstants(&prepPush, sizeof(prepPush));
    m_prepareIndirect->setGroupCount(WavefrontStage::Fixed{1u});
    m_prepareIndirect->record(cmd);

    // (5) COMPUTE -> DRAW_INDIRECT. vkCmdDispatchIndirect reads the triple
    // just written, and that read happens in the DRAW_INDIRECT stage -- not
    // COMPUTE_SHADER, not HOST. Omitting this is invalid usage that produces
    // no diagnostic on this hardware; Stage 0b-1 deleted the equivalent
    // barrier on purpose and watched the survivor count collapse to 0 with
    // zero SYNC- messages emitted.
    //
    // dstAccessMask is INDIRECT_COMMAND_READ ALONE. That is correct only
    // because kIndirectArgsSlot (slots 2-4) is disjoint from the slots this
    // loop ping-pongs (0 and 1) and from kCanarySlot (5): the dispatch's own
    // reads of src.countSlot and atomicAdds on dst.countSlot touch different
    // bytes of this buffer than prepare_indirect wrote, and are ordered by
    // program order plus the fact that vkCmdDispatchIndirect starts no
    // invocations until its indirect read completes. That invariant holds by
    // luck of the slot layout, not by construction -- see the class comment.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT, &counter, 1);

    // (6) The compacting dispatch itself, sized from the triple above.
    stage.setGroupCount(WavefrontStage::Indirect{counter, argsOffset});
    stage.record(cmd);

    // (7) COMPUTE -> COMPUTE over all three buffers.
    //
    // This is what makes bounce k read what bounce k-1 wrote. The dispatch
    // just recorded wrote path state, the destination queue ring, and
    // counter slots (dst.countSlot via atomicAdd, plus kCanarySlot for
    // intersect); the NEXT compacting stage reads all three -- its
    // prepare_indirect reads the count slot this one produced, and it reads
    // the queue ring this one compacted into. dstAccessMask includes
    // SHADER_WRITE because that next stage atomicAdds, and because a
    // read-only destination would not order the write-after-write on state.
    //
    // KNOWN GAP -- MEASURED, NOT ASSUMED, AND BROADER THAN IT LOOKS. Task 3's
    // required proof first deleted just this barrier (7) -- skipped after
    // intersect only, so exactly one SHADER_WRITE -> SHADER_READ|SHADER_WRITE
    // barrier between intersect and scatter was missing. NOTHING failed. But
    // that experiment was largely a no-op: skipping (7) leaves the
    // *execution* dependency between intersect and scatter fully intact,
    // because the FOLLOWING recordCompactingStage's own barriers (1)
    // (COMPUTE -> TRANSFER) and (3) (TRANSFER -> COMPUTE) still serialise the
    // two dispatches -- a VkBufferMemoryBarrier narrows the *memory* scope of
    // a dependency, never the *execution* scope. It also leaves the entire
    // counter-buffer memory dependency intact, since (1) and (3) form a
    // matching chain. Only the state and queue buffers' visibility was
    // actually removed.
    //
    // The real measurement is the stronger one: disabling EVERY compute-side
    // memory barrier in the loop -- the post-generate barrier (barrier A,
    // COMPUTE -> COMPUTE, immediately after m_generate->record(cmd) in
    // record() below), plus (1), (3) and this barrier (7), each
    // recorded once per compacting dispatch (eight times over a 4-bounce
    // run) -- leaving only the vkCmdFillBuffer calls and barrier (5),
    // COMPUTE -> DRAW_INDIRECT, standing. Result: exit=0, ok=22, sync=0,
    // three consecutive runs. The contrast -- deleting (5) ALONE, first
    // occurrence only -- fails immediately: "fused loop of 1 bounces left 0
    // live paths, expected all 512". So (5) is covered; barrier A, (1), (3)
    // and (7) are not. Neither the analytic checks (throughput == 0.0625
    // bit-exact, per-bounce PathRng parity, one-of-each compaction ring) nor
    // synchronization validation covers any of this compute-to-compute /
    // transfer-to-compute barrier set on this hardware -- the driver happens
    // to serialise back-to-back compute/transfer work on one queue anyway.
    // Every one of those barriers is kept because the Vulkan spec requires
    // it, not because anything here would notice its absence. Do not
    // conclude from a green test run that this barrier set can go. See
    // task-3-report.md.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         kShaderReadWrite, allBuffers, 3);

    // One WAR ordering in this function is spelled out nowhere else, so it
    // is worth naming even though no barrier had to change for it: dispatch
    // N's vkCmdDispatchIndirect reads counter slots kIndirectArgsSlot (2-4)
    // in the DRAW_INDIRECT stage (see barrier (5) above), and the NEXT
    // compacting dispatch's prepare_indirect (step (4), next call to this
    // function) does a SHADER_WRITE to those SAME slots -- kIndirectArgsSlot
    // is not ping-ponged, it is reused by every compacting stage. That WAR
    // is ordered, but only transitively: this barrier's srcStageMask is
    // COMPUTE_SHADER, and per the Vulkan spec a stage named in srcStageMask
    // brings every logically-earlier stage OF THE SAME COMMAND into the
    // first synchronization scope too. DRAW_INDIRECT is logically earlier
    // than COMPUTE_SHADER for one dispatch, so dispatch N's indirect-args
    // read is captured here even though this barrier never names
    // DRAW_INDIRECT explicitly. From here the ordering continues by plain
    // program order through (1) and (3) of the next recordCompactingStage
    // call to prepare_indirect's write. This is valid Vulkan usage, just the
    // one ordering in this file a reviewer could miss by reading srcStageMask
    // literally instead of per the spec's logically-earlier-stages rule.
}

void WavefrontLoop::record(VkCommandBuffer cmd, WavefrontBuffers& buffers,
                           std::uint32_t bounces) const {
    assert(isComplete() && "WavefrontLoop::record: all four stages must be set");

    // Fail closed, unconditionally (the assert above is compiled out under
    // NDEBUG, which this repo's Release test targets define) -- same
    // "assert, then unconditional guard" pattern WavefrontStage::record and
    // ArenaLayout::block use. Recording half a loop would be worse than
    // recording none of it: a partially-recorded bounce produces state that
    // looks like a barrier bug.
    if (!isComplete()) return;

    const std::uint32_t capacity = buffers.layout().capacity();
    const VkBuffer state = buffers.stateBuffer();
    const VkBuffer queue = buffers.queueBuffer();
    const VkBuffer counter = buffers.counterBuffer();
    assert(capacity > 0 && state != VK_NULL_HANDLE && queue != VK_NULL_HANDLE &&
          counter != VK_NULL_HANDLE && "WavefrontLoop::record: buffers must be built");
    if (capacity == 0 || state == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
       counter == VK_NULL_HANDLE) {
        return;
    }

    const VkBuffer allBuffers[3] = {state, queue, counter};

    // --- generate: one path per pixel into ring 0 / kCurrentCountSlot. ---
    // Its push constants and group count belong to the caller (the dispatch
    // shape is a property of the image, and neither changes between
    // bounces), so this only records it.
    m_generate->record(cmd);

    // generate's writes -- path state, queue ring 0, and counter slot 0 via
    // atomicAdd -- are read by the first bounce's prepare_indirect (the
    // count) and intersect (state + ring). SHADER_WRITE -> SHADER_READ |
    // SHADER_WRITE, COMPUTE -> COMPUTE.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         kShaderReadWrite, allBuffers, 3);

    // Counter -> DRAW_INDIRECT as well. Nothing generate wrote is read as an
    // indirect command before the first prepare_indirect overwrites the args
    // slot, so on its own this barrier is not load-bearing; it is recorded
    // because the loop's contract is "the counter buffer is coherent for
    // indirect reads at every bounce boundary", and having exactly one place
    // where that stops being true is how the rule gets quietly broken later.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT, &counter, 1);

    // --- The bounce loop. ---
    //
    // src is where the live path list currently is; dst is the other ring.
    // The pair is swapped after EVERY compacting dispatch, not once per
    // bounce, because a bounce contains two of them. See the class comment:
    // tying the swap to the compaction that causes it is what removes any
    // place for the ping-pong phase to drift by one.
    Ring src{0u, WavefrontBuffers::kCurrentCountSlot};
    Ring dst{capacity, WavefrontBuffers::kNextCountSlot};

    for (std::uint32_t b = 0; b < bounces; ++b) {
        // Trace + compact survivors: src -> dst.
        const IntersectPush intersectPush{capacity,      src.queueBase, src.countSlot,
                                          dst.queueBase, dst.countSlot,
                                          WavefrontBuffers::kCanarySlot};
        m_intersect->setPushConstants(&intersectPush, sizeof(intersectPush));
        recordCompactingStage(cmd, buffers, *m_intersect, src, dst);
        std::swap(src, dst);

        // Shade + re-queue: src (which is now what intersect just produced)
        // -> dst.
        const ScatterPush scatterPush{capacity,      src.queueBase, src.countSlot,
                                      dst.queueBase, dst.countSlot, m_config.albedo,
                                      m_config.iterationSeed};
        m_scatter->setPushConstants(&scatterPush, sizeof(scatterPush));
        recordCompactingStage(cmd, buffers, *m_scatter, src, dst);
        std::swap(src, dst);
    }

    // No trailing host/transfer-visibility barrier: only the caller knows
    // what consumes the result (see record()'s doc comment).
}

}  // namespace ohao::diff
