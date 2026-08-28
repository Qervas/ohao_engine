#include "diff/wavefront/wavefront_loop.hpp"

#include <cassert>
#include <span>
#include <utility>
#include <vector>

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

/// How many barrier targets fit without touching the heap. Three is what the
/// loop's own buffers need (state, queue, counter); the rest is headroom for
/// `record()`'s caller-owned extras (see WavefrontLoop::record's
/// `extraBarrierBuffers`). This is a PERFORMANCE bound only -- exceeding it
/// spills to a vector, it never drops a barrier. See the note below.
constexpr std::size_t kInlineBarrierTargets = 8;

/// Records one VkBufferMemoryBarrier per entry of `targets` -- however many
/// that is -- all with the same stage/access scopes and all covering the
/// whole buffer.
///
/// `targets.size()` is NOT capped. An earlier version wrote into a
/// `VkBufferMemoryBarrier[3]` behind `assert(count <= 3)` plus a
/// `if (count > 3) count = 3;` clamp, which under NDEBUG -- which this repo's
/// Release test targets define, so it is the configuration that actually
/// ships -- silently dropped every barrier past the third. A dropped memory
/// barrier in this loop is exactly the class of defect nothing here detects
/// (see wavefront_loop.hpp's measurement: every compute-side barrier can be
/// deleted at once and all checks still pass), so a silent clamp is the worst
/// available failure mode. The inline array is now only an allocation
/// optimisation: anything larger spills to `heap`, and the emitted barrier
/// count always equals `targets.size()`.
///
/// Whole-buffer rather than per-slot ranges on purpose: the counter buffer's
/// slots are only 4 bytes apart and several of them are touched by different
/// accesses within one bounce, so a byte-range-precise barrier set would be
/// far harder to read and review than the ordering it encodes -- and human
/// review is the only check this code gets (see wavefront_loop.hpp).
void recordBufferBarriers(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess, std::span<const VkBuffer> targets) {
    VkBufferMemoryBarrier inlineBarriers[kInlineBarrierTargets]{};
    std::vector<VkBufferMemoryBarrier> heap;
    VkBufferMemoryBarrier* barriers = inlineBarriers;
    if (targets.size() > kInlineBarrierTargets) {
        heap.resize(targets.size());
        barriers = heap.data();
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcAccessMask = srcAccess;
        barriers[i].dstAccessMask = dstAccess;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].buffer = targets[i];
        barriers[i].offset = 0;
        barriers[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr,
                         static_cast<uint32_t>(targets.size()), barriers, 0, nullptr);
}

/// Builds the target list for a barrier that has to cover the loop's own
/// three buffers AND every caller-owned buffer the caller folded in. The
/// loop's three always come FIRST and in the same order (state, queue,
/// counter), so adding extras cannot perturb the barrier entries that were
/// already there -- only append to them.
std::vector<VkBuffer> withExtras(const VkBuffer (&own)[3], std::span<const VkBuffer> extras) {
    std::vector<VkBuffer> targets;
    targets.reserve(3 + extras.size());
    targets.insert(targets.end(), own, own + 3);
    for (VkBuffer b : extras) {
        if (b != VK_NULL_HANDLE) targets.push_back(b);
    }
    return targets;
}

}  // namespace

void recordIndirectSizedDispatch(VkCommandBuffer cmd, VkBuffer counter, std::uint32_t srcCountSlot,
                                 WavefrontStage& prepare, WavefrontStage& work) {
    // (4) prepare_indirect: counter[srcCountSlot] -> the {groupCountX,1,1}
    // VkDispatchIndirectCommand triple at kIndirectArgsSlot, in this same
    // counter buffer. The live count exists only on the GPU timeline;
    // reading it back to size a dispatch on the host would serialise every
    // caller on a round-trip and defeat the point of an indirect dispatch.
    const WavefrontLoop::PrepareIndirectPush prepPush{srcCountSlot,
                                                       WavefrontBuffers::kIndirectArgsSlot};
    prepare.setPushConstants(&prepPush, sizeof(prepPush));
    prepare.setGroupCount(WavefrontStage::Fixed{1u});
    prepare.record(cmd);

    // (5) COMPUTE -> DRAW_INDIRECT. vkCmdDispatchIndirect (inside
    // work.record below) reads the triple just written, and that read
    // happens in the DRAW_INDIRECT stage -- not COMPUTE_SHADER, not HOST.
    // Omitting this is invalid usage that produces no diagnostic on this
    // hardware; Stage 0b-1 deleted the equivalent barrier on purpose and
    // watched the survivor count collapse to 0 with zero SYNC- messages
    // emitted. This is the ONLY barrier in the wavefront subsystem whose
    // absence anything in this repository's checks currently detects -- see
    // WavefrontLoop's class comment and task-3-report.md for the
    // measurement that established that.
    //
    // dstAccessMask is INDIRECT_COMMAND_READ ALONE. That is correct only
    // because kIndirectArgsSlot (slots 2-4) is disjoint from every slot any
    // caller ping-pongs and from kCanarySlot (5): the dispatch's own reads
    // of srcCountSlot and atomicAdds on a destination slot touch different
    // bytes of this buffer than prepare_indirect wrote, and are ordered by
    // program order plus the fact that vkCmdDispatchIndirect starts no
    // invocations until its indirect read completes. That invariant holds
    // by luck of the slot layout, not by construction -- see
    // WavefrontLoop's "Counter-slot layout invariant" section.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                         std::span<const VkBuffer>(&counter, 1));

    // (6) The sized dispatch itself, sized from the triple above.
    const VkDeviceSize argsOffset =
        static_cast<VkDeviceSize>(WavefrontBuffers::kIndirectArgsSlot) * sizeof(std::uint32_t);
    work.setGroupCount(WavefrontStage::Indirect{counter, argsOffset});
    work.record(cmd);
}

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
                                          WavefrontStage& stage, const Ring& src, const Ring& dst,
                                          std::span<const VkBuffer> extraBarrierBuffers) const {
    const VkBuffer counter = buffers.counterBuffer();
    const VkBuffer allBuffers[3] = {buffers.stateBuffer(), buffers.queueBuffer(), counter};
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
                         kShaderReadWrite, VK_ACCESS_TRANSFER_WRITE_BIT,
                         std::span<const VkBuffer>(&counter, 1));

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
                         VK_ACCESS_TRANSFER_WRITE_BIT, kShaderReadWrite,
                         std::span<const VkBuffer>(&counter, 1));

    // (4)-(6): prepare_indirect (counter[src.countSlot] -> the
    // {groupCountX,1,1} triple at kIndirectArgsSlot), the
    // COMPUTE -> DRAW_INDIRECT barrier ordering that write before
    // vkCmdDispatchIndirect reads it -- the ONLY barrier in this subsystem
    // whose absence anything in this repository's checks currently detects,
    // see the class comment -- and the compacting dispatch itself, sized
    // from that triple. Identical at every call site that sizes a dispatch
    // this way (this function and gpu_probe_context.cpp's
    // runWavefrontIntersectProbe/runWavefrontScatterProbe), so it lives in
    // exactly one place: recordIndirectSizedDispatch's doc comment in
    // wavefront_loop.hpp has the full account of why each piece is required.
    recordIndirectSizedDispatch(cmd, counter, src.countSlot, *m_prepareIndirect, stage);

    // (7) COMPUTE -> COMPUTE over all three buffers, plus every buffer in
    // `extraBarrierBuffers` (appended after the three, never reordering
    // them).
    //
    // The extras are not decoration. This barrier supplies the *execution*
    // dependency for everything the dispatch wrote, but availability and
    // visibility are restricted to the buffers named in its
    // VkBufferMemoryBarrier list -- the same memory-scope/execution-scope
    // distinction the KNOWN GAP note below turns on. wf_scatter.comp writes
    // debugDraws.v[pathIndex*3 + 0..2] at a FIXED offset on EVERY bounce, so
    // in a fused run of bounces >= 2 successive scatter dispatches overwrite
    // the same bytes and only this list makes bounce k's write available
    // before bounce k+1's. See wavefront_loop.hpp's "Caller-owned buffers
    // the loop must also order".
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
                         kShaderReadWrite, withExtras(allBuffers, extraBarrierBuffers));

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
                           std::uint32_t bounces,
                           std::span<const VkBuffer> extraBarrierBuffers) const {
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

    // (A) generate's writes -- path state, queue ring 0, and counter slot 0
    // via atomicAdd -- are read by the first bounce's prepare_indirect (the
    // count) and intersect (state + ring). SHADER_WRITE -> SHADER_READ |
    // SHADER_WRITE, COMPUTE -> COMPUTE.
    //
    // `extraBarrierBuffers` is folded in here as well as at (7). generate
    // writes none of them today, so this is not load-bearing on its own; it
    // is recorded for the same reason as the DRAW_INDIRECT barrier just
    // below, namely that "every caller-owned output is coherent at every
    // bounce boundary, including the one before bounce 0" is a rule with no
    // exceptions to remember. A generate stage that one day seeds a film or
    // radiance buffer would otherwise be a silent hazard.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         kShaderReadWrite, withExtras(allBuffers, extraBarrierBuffers));

    // Counter -> DRAW_INDIRECT as well. Nothing generate wrote is read as an
    // indirect command before the first prepare_indirect overwrites the args
    // slot, so on its own this barrier is not load-bearing; it is recorded
    // because the loop's contract is "the counter buffer is coherent for
    // indirect reads at every bounce boundary", and having exactly one place
    // where that stops being true is how the rule gets quietly broken later.
    recordBufferBarriers(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
                         std::span<const VkBuffer>(&counter, 1));

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
        recordCompactingStage(cmd, buffers, *m_intersect, src, dst, extraBarrierBuffers);
        std::swap(src, dst);

        // Shade + re-queue: src (which is now what intersect just produced)
        // -> dst.
        const ScatterPush scatterPush{capacity,
                                      src.queueBase,
                                      src.countSlot,
                                      dst.queueBase,
                                      dst.countSlot,
                                      m_config.albedo,
                                      m_config.iterationSeed,
                                      m_config.roughness,
                                      m_config.metallic,
                                      m_config.specularWeight,
                                      buffers.envWidth(),
                                      buffers.envHeight(),
                                      buffers.envIntegral(),
                                      // Film: from Config, not from
                                      // `buffers` -- the film is
                                      // caller-owned and WavefrontBuffers
                                      // cannot state its size. 0 disables
                                      // the accumulation, which is what
                                      // every caller that passes no film
                                      // buffer gets.
                                      m_config.filmPixelCount,
                                      // Gradient arena: from Config, for the
                                      // film's reason -- caller-owned, so
                                      // `buffers` cannot state its size. 0
                                      // floats disables every gradient write,
                                      // which is what every caller that binds
                                      // a placeholder buffer at binding 10
                                      // gets.
                                      m_config.gradArenaFloats,
                                      m_config.gradAlbedoOffset,
                                      // The detached-sampling material
                                      // (Stage 1 Task 3), passed through
                                      // verbatim. A negative
                                      // `samplingRoughness` is the "sample
                                      // with the evaluated material" default
                                      // and is resolved by the TRAVERSAL, not
                                      // here -- see ScatterPush's note on why
                                      // the sentinel has to survive as far as
                                      // the shader.
                                      m_config.samplingAlbedo,
                                      m_config.samplingRoughness,
                                      m_config.samplingMetallic,
                                      m_config.samplingSpecularWeight,
                                      m_config.diffParam};
        m_scatter->setPushConstants(&scatterPush, sizeof(scatterPush));
        recordCompactingStage(cmd, buffers, *m_scatter, src, dst, extraBarrierBuffers);
        std::swap(src, dst);
    }

    // No trailing host/transfer-visibility barrier: only the caller knows
    // what consumes the result (see record()'s doc comment).
}

}  // namespace ohao::diff
