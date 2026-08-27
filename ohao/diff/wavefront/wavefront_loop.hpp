#pragma once

#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace ohao::diff {

/// Records the ENTIRE wavefront bounce loop -- generate, then
/// prepare_indirect/intersect/prepare_indirect/scatter once per bounce --
/// into ONE command buffer, with no host round-trip and no
/// `vkQueueWaitIdle` anywhere inside it.
///
/// This class is where the wavefront integrator stops being a sequence of
/// individually-verified probes and becomes a pipeline. Stage 0b-1 proved
/// each shader correct in isolation, but every probe there submitted its
/// own command buffer and waited for the device to go idle between stages,
/// so *ordering* was never actually the barriers' job -- the idle wait made
/// every hazard unobservable. Fusing the loop removes that wait. From here
/// on, the only thing standing between a correct bounce and silently
/// corrupt path state is the barrier set recorded below.
///
/// That is why every barrier lives in this one file rather than being
/// scattered into WavefrontStage::record (which deliberately records no
/// barriers at all -- see its class comment): the full ordering contract of
/// the loop has to be reviewable by a human reading a single function.
/// It cannot be delegated to a tool. Synchronization validation has been
/// MEASURED not to catch the hazards this loop depends on: in Stage 0b-1
/// the counter's SHADER_WRITE -> INDIRECT_COMMAND_READ barrier was deleted
/// on purpose, the survivor count collapsed from 1536 to 0 -- an outright
/// correctness failure -- and the validation layers emitted exactly zero
/// `SYNC-` diagnostics. A clean validation run is not evidence that the
/// barriers here are right.
///
/// ### Known gap: the checks do not cover the inter-dispatch barrier
///
/// Task 3 measured this too, by deleting the single
/// `SHADER_WRITE -> SHADER_READ|SHADER_WRITE` barrier between intersect and
/// scatter and re-running the probe. NOTHING failed -- every analytic check
/// still passed bit-exactly over six consecutive runs, and validation again
/// emitted zero `SYNC-` messages. That barrier is required by the Vulkan
/// spec and it is recorded below, but on this hardware nothing in this
/// repository would notice if it went missing: the driver happens to
/// serialise back-to-back compute dispatches on one queue regardless. Treat
/// a green `diff_gpu_probe` as evidence about the *shaders*, never as
/// evidence about the compute-to-compute barriers. See task-3-report.md.
///
/// ### The three ordering rules this loop is built on
///
/// 1. **`dstAccessMask` must include `SHADER_WRITE`, not just
///    `SHADER_READ`, wherever the next stage does an `atomicAdd` on a value
///    the previous access wrote.** An atomic is a read-modify-*write*; a
///    read-only destination scope does not order it. Both compacting stages
///    (`wf_intersect.comp`, `wf_scatter.comp`) atomicAdd on a counter slot,
///    so every barrier that has to be visible to one of them names
///    `SHADER_READ | SHADER_WRITE`.
///
/// 2. **`INDIRECT_COMMAND_READ` at `DRAW_INDIRECT` is required before every
///    indirect dispatch.** `vkCmdDispatchIndirect` reads the
///    `VkDispatchIndirectCommand` triple `wf_prepare_indirect.comp` just
///    wrote to the SAME counter buffer, and that read happens in the
///    `DRAW_INDIRECT` stage, not in `COMPUTE_SHADER`. Omitting the barrier
///    is invalid usage that produces no diagnostic on this hardware.
///
/// 3. **The compaction destination counter slot must be zeroed before the
///    dispatch that atomicAdds into it.** See "Zeroing the destination
///    slot" below -- this is not an optimisation, it is the difference
///    between correct compaction offsets and silently displaced ones.
///
/// ### Ring / counter-slot ping-pong
///
/// `WavefrontBuffers` holds exactly two queue rings (element bases `0` and
/// `capacity`) and two live-count slots (`kCurrentCountSlot` = 0,
/// `kNextCountSlot` = 1). A bounce runs TWO compacting dispatches, not one:
/// intersect compacts survivors of the trace, then scatter re-queues what
/// survived scattering. So the (source, destination) pair is swapped after
/// EACH compacting dispatch, not once per bounce:
///
/// ```
///   generate                 -> writes ring 0 / slot 0
///   bounce k: intersect  ring 0 / slot 0 -> ring 1 / slot 1   [swap]
///             scatter    ring 1 / slot 1 -> ring 0 / slot 0   [swap]
/// ```
///
/// Expressing it as "swap after every compaction" rather than "swap once
/// per bounce" is deliberate: an off-by-one here makes bounce *k* consume
/// bounce *k-1*'s survivor list, which still traces, still produces
/// plausible counts, and is silently wrong. With the swap tied to the
/// compaction that causes it, there is no separate place for the phase to
/// drift out of step.
///
/// Two swaps per bounce is an even number, so the live path list is always
/// back in ring 0 / slot 0 when a bounce ends -- see `finalLiveRing()`,
/// which exists so callers read that fact from the loop rather than
/// hardcoding an assumption they cannot verify.
///
/// ### Zeroing the destination slot
///
/// Each compacting dispatch hands out destination offsets with
/// `atomicAdd(counters[dstCountSlot], 1)`, so that slot must be 0 going in.
/// It generally is not: under ping-pong, the destination slot is a slot
/// some earlier dispatch already counted into. Concretely, at bounce 0
/// intersect consumes slot 0 (generate's count) and produces into slot 1;
/// scatter then produces into slot 0 -- which still holds generate's
/// original, non-zero count. Every compaction offset from that point on is
/// displaced by it. Paths still trace, counts still look plausible, and the
/// corruption is completely silent. `gpu_probe_context.cpp`'s stage-by-
/// stage scatter probe zeroes its destination slot for exactly this reason.
///
/// So each compacting dispatch here is preceded by a
/// `vkCmdFillBuffer(dstSlot, 0)` and a `TRANSFER_WRITE ->
/// SHADER_READ|SHADER_WRITE` barrier.
///
/// ### The hazard fusing this loop creates
///
/// Those fills put a TRANSFER-stage write BETWEEN two compute dispatches in
/// one command buffer, which is a configuration no Stage 0b-1 probe ever
/// recorded. The fill overwrites a slot the PRECEDING dispatch reads (at
/// bounce 1, intersect's destination slot 1 is the very slot bounce 0's
/// scatter read as its source count). Nothing orders that write against
/// that read except a barrier: the old code did not need one only because
/// `vkQueueWaitIdle` separated every probe, and removing that wait is the
/// entire point of this class. Hence the
/// `SHADER_READ|SHADER_WRITE -> TRANSFER_WRITE`
/// (`COMPUTE_SHADER -> TRANSFER`) barrier before each fill. No existing
/// code demonstrates it and no current test would catch its absence.
///
/// ### Counter-slot layout invariant (do not break)
///
/// The pre-indirect barrier names `INDIRECT_COMMAND_READ` **alone** in its
/// `dstAccessMask` -- not also `SHADER_READ|SHADER_WRITE` for the consuming
/// stage's own reads of the source count slot and its atomicAdds on the
/// destination/canary slots. That is correct only because
/// `kIndirectArgsSlot` (slots 2-4) is disjoint from every slot this loop
/// ping-pongs (0 and 1) and from `kCanarySlot` (5): those accesses touch
/// different bytes of the counter buffer than `wf_prepare_indirect.comp`
/// wrote, and are ordered by program order plus the fact that
/// `vkCmdDispatchIndirect` starts no invocations until its indirect read
/// completes. The invariant holds by luck of the slot layout, not by
/// construction. Anything that reassigns counter slots so that a
/// ping-ponged slot overlaps 2-4 silently reintroduces a hazard this
/// barrier does not cover. Slots 6 and 7 are free; use those.
///
/// ### Ownership
///
/// The four stages are referenced, not owned. `WavefrontStage` deletes both
/// copy and move (it holds raw Vulkan handles it is responsible for
/// destroying), so a `setGenerate(WavefrontStage)` taking its argument by
/// value -- as the task brief sketches it -- cannot compile. Taking a
/// reference and storing a non-owning pointer is the only shape available,
/// and it is also the right one: the stages outlive any single `record()`
/// and are rebuilt/destroyed on the caller's schedule, not this class's.
class WavefrontLoop {
public:
    /// Per-run parameters that do not change between bounces. Everything
    /// that DOES change between bounces (queue bases, counter slots) is
    /// derived inside `record()` -- see the ping-pong note above -- so that
    /// no caller can get the phase wrong.
    ///
    /// `capacity` is not here: it comes from the `WavefrontBuffers` passed
    /// to `record()`, which is the only object that can state it
    /// authoritatively.
    struct Config {
        /// Constant albedo `wf_scatter.comp` multiplies throughput by every
        /// bounce. There is no real BSDF yet (Stage 0b-2).
        float albedo{0.5f};
        /// Per-iteration RNG seed. Combined with (pixelIndex, sampleIndex)
        /// and the path's stored bounce count, this is the ONLY thing a
        /// scatter dispatch reconstructs its random stream from -- nothing
        /// survives in registers across a dispatch boundary.
        std::uint32_t iterationSeed{0};
    };

    /// One end of the ping-pong: a queue ring's element base and the
    /// counter slot holding that ring's live-path count.
    struct Ring {
        std::uint32_t queueBase{0};
        std::uint32_t countSlot{0};
    };

    // --- Push blocks, byte-matched to the GLSL they are pushed to. ---
    // Declared here rather than in each caller because record() is what
    // fills the ping-ponged fields in, and a mismatch between these and the
    // shader's Push block is a silent wrong-slot read, not a validation
    // error.

    /// Matches `shaders/diff/wf_prepare_indirect.comp`'s Push block.
    struct PrepareIndirectPush {
        std::uint32_t countSlot;
        std::uint32_t argsSlot;
    };

    /// Matches `shaders/diff/wf_intersect.comp`'s Push block.
    struct IntersectPush {
        std::uint32_t capacity;
        std::uint32_t srcQueueBase;
        std::uint32_t srcCountSlot;
        std::uint32_t dstQueueBase;
        std::uint32_t dstCountSlot;
        std::uint32_t canarySlot;
    };

    /// Matches `shaders/diff/wf_scatter.comp`'s Push block.
    struct ScatterPush {
        std::uint32_t capacity;
        std::uint32_t srcQueueBase;
        std::uint32_t srcCountSlot;
        std::uint32_t dstQueueBase;
        std::uint32_t dstCountSlot;
        float albedo;
        std::uint32_t iterationSeed;
    };

    WavefrontLoop() = default;

    void setConfig(const Config& config) noexcept { m_config = config; }

    // The stages must outlive every record() call made against this loop.
    // Each must have been built and bound (see WavefrontStage::record's
    // precondition) before record() is called; a stage that fails that
    // precondition no-ops rather than recording an invalid command, which
    // would turn the whole loop into a silent no-op -- hence isComplete()
    // and the fail-closed guard in record().
    void setGenerate(WavefrontStage& stage) noexcept { m_generate = &stage; }
    void setPrepareIndirect(WavefrontStage& stage) noexcept { m_prepareIndirect = &stage; }
    void setIntersect(WavefrontStage& stage) noexcept { m_intersect = &stage; }
    void setScatter(WavefrontStage& stage) noexcept { m_scatter = &stage; }

    /// True once all four stages have been set. record() is a no-op
    /// otherwise.
    [[nodiscard]] bool isComplete() const noexcept {
        return m_generate != nullptr && m_prepareIndirect != nullptr && m_intersect != nullptr &&
               m_scatter != nullptr;
    }

    /// Records generate + `bounces` complete bounces into `cmd`.
    ///
    /// The caller is responsible for (a) zeroing `buffers` before this --
    /// `WavefrontBuffers::zero` may be recorded into the same command
    /// buffer immediately before, it ends with its own
    /// TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier -- and (b) any
    /// barrier needed to make the results visible AFTER this (host reads,
    /// transfer copies). record() emits nothing at either end for those:
    /// the loop's own barriers order the loop against itself, and only the
    /// caller knows what consumes the result.
    ///
    /// The caller also owns generate's push constants and group count:
    /// they do not change between bounces, and generate's dispatch shape is
    /// a property of the image being rendered, not of the loop.
    ///
    /// `record()` IS const while mutating the stages' push constants and
    /// group-count sources through the stored pointers: those are the
    /// stages' recording state, not this loop's configuration. It is safe
    /// to call more than once (WavefrontStage::setPushConstants deep-copies
    /// and vkCmdPushConstants captures into the command buffer at record
    /// time), but the stages must not be recorded concurrently from another
    /// thread.
    void record(VkCommandBuffer cmd, WavefrontBuffers& buffers, std::uint32_t bounces) const;

    /// Where the live path list ends up after `bounces` bounces. Two
    /// compactions per bounce means an even number of swaps, so this is
    /// always ring 0 / `kCurrentCountSlot` -- but callers read it from here
    /// rather than hardcoding it, so that a loop which one day grows an odd
    /// number of compacting stages updates exactly one place.
    [[nodiscard]] static Ring finalLiveRing(std::uint32_t capacity,
                                            std::uint32_t bounces) noexcept;

private:
    /// Records one compacting dispatch and everything that has to surround
    /// it: the destination-slot zero-fill (with barriers on BOTH sides of
    /// it), the prepare_indirect dispatch that sizes the dispatch from
    /// `src.countSlot`, the pre-indirect barrier, the dispatch itself, and
    /// the post-dispatch barrier that makes its writes visible to whatever
    /// stage comes next.
    ///
    /// `stage` is already loaded with its push constants by the caller;
    /// this function sets its group-count source to the indirect args and
    /// records it.
    void recordCompactingStage(VkCommandBuffer cmd, const WavefrontBuffers& buffers,
                               WavefrontStage& stage, const Ring& src, const Ring& dst) const;

    Config m_config{};
    WavefrontStage* m_generate{nullptr};
    WavefrontStage* m_prepareIndirect{nullptr};
    WavefrontStage* m_intersect{nullptr};
    WavefrontStage* m_scatter{nullptr};
};

}  // namespace ohao::diff
