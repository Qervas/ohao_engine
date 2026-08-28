#pragma once

#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace ohao::diff {

/// Records the ONE step sequence that turns a live-path count already
/// sitting in `counter[srcCountSlot]` into a completed, GPU-sized indirect
/// dispatch of `work` -- without any host round-trip to read that count:
///
///  1. Records `prepare` (`shaders/diff/wf_prepare_indirect.comp`) with a
///     freshly-set `WavefrontLoop::PrepareIndirectPush{srcCountSlot,
///     WavefrontBuffers::kIndirectArgsSlot}` and group count `Fixed{1u}`.
///     This converts the count at `counter[srcCountSlot]` into a
///     `{groupCountX,1,1}` `VkDispatchIndirectCommand` triple at
///     `counter[kIndirectArgsSlot..+2]`.
///  2. A `COMPUTE_SHADER -> DRAW_INDIRECT` / `SHADER_WRITE ->
///     INDIRECT_COMMAND_READ` buffer barrier over the whole of `counter`,
///     ordering that write before `vkCmdDispatchIndirect` (inside `work`'s
///     record, next) reads it. `vkCmdDispatchIndirect` reads the triple in
///     the `DRAW_INDIRECT` stage, not `COMPUTE_SHADER`, so omitting this is
///     invalid usage -- and on this hardware it produces no diagnostic: see
///     the measurement in `WavefrontLoop`'s class comment. **This is the
///     ONLY barrier in the wavefront subsystem whose absence anything in
///     this repository's checks currently detects.**
///  3. Records `work` with its group-count source set to
///     `Indirect{counter, kIndirectArgsSlot's byte offset}`, i.e. sized by
///     the triple step 1 produced and step 2 just made visible.
///
/// `work`'s OWN push constants (e.g. `WavefrontLoop::IntersectPush` /
/// `ScatterPush`) are the caller's responsibility -- set them before calling
/// this function. This function only ever touches `prepare`'s push
/// constants/group count and `work`'s group count.
///
/// Extracted because this exact sequence used to be hand-copied at three
/// call sites -- `WavefrontLoop::recordCompactingStage`, and
/// `tests/diff/gpu_probe_context.cpp`'s `runWavefrontIntersectProbe` and
/// `runWavefrontScatterProbe` -- which record it into otherwise-different
/// command buffers (a fused multi-bounce loop vs. a fresh command buffer per
/// probe with `vkQueueWaitIdle` before and a host-readback tail after).
/// Three hand-maintained copies of the one barrier anything here actually
/// verifies is worse than one shared function; the surrounding context
/// (what precedes `srcCountSlot` being valid, what consumes `work`'s writes
/// afterwards) legitimately differs per call site and stays there.
void recordIndirectSizedDispatch(VkCommandBuffer cmd, VkBuffer counter, std::uint32_t srcCountSlot,
                                 WavefrontStage& prepare, WavefrontStage& work);

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
/// ### Known gap: the checks do not cover the compute/transfer barrier set
///
/// Task 3 first tried deleting a single barrier -- `SHADER_WRITE ->
/// SHADER_READ|SHADER_WRITE` between intersect and scatter (barrier (7) in
/// `recordCompactingStage`, skipped after intersect only). NOTHING failed.
/// But that result is largely a no-op, not evidence the barrier is unneeded:
/// skipping it leaves the *execution* dependency between the two dispatches
/// fully intact, because the FOLLOWING compacting stage's own barriers (1)
/// (`COMPUTE -> TRANSFER`) and (3) (`TRANSFER -> COMPUTE`), both present and
/// forming a valid chain, still serialise them -- a `VkBufferMemoryBarrier`
/// narrows the *memory* scope of a dependency, it never narrows the
/// *execution* scope. It also leaves the whole counter-buffer memory
/// dependency intact, since (1) and (3) match scope-for-scope. Only the
/// state and queue buffers' visibility was actually removed by that one
/// deletion.
///
/// The real measurement is the stronger one: disabling EVERY compute-side
/// memory barrier in the loop -- the post-generate barrier (barrier A,
/// `COMPUTE -> COMPUTE`, recorded right after `generate` in `record()`)
/// plus (1), (3) and (7) inside
/// `recordCompactingStage` (each recorded once per compacting dispatch, so
/// eight times over in a 4-bounce run), leaving only the `vkCmdFillBuffer`
/// calls and the `COMPUTE_SHADER -> DRAW_INDIRECT` barrier (5) standing --
/// and re-running the probe. NOTHING failed there either, over three
/// consecutive runs, all `exit=0`, `ok=22`, `sync=0`. The contrast: deleting
/// barrier (5) alone (first occurrence only) fails immediately --
/// `fused loop of 1 bounces left 0 live paths, expected all 512` -- so (5)
/// IS covered.
///
/// So: **on this driver, the ONLY barrier in this loop whose absence these
/// checks detect is (5), `COMPUTE_SHADER -> DRAW_INDIRECT` /
/// `INDIRECT_COMMAND_READ`.** The entire compute-to-compute and
/// transfer-to-compute barrier set -- barrier A, (1), (3) and (7) -- is
/// unguarded by anything in this repository, and synchronization validation
/// stays silent for all of it. Every one of those barriers is required by
/// the Vulkan spec and is recorded below, but nothing here would notice if
/// any of them went missing: the driver happens to serialise back-to-back
/// compute/transfer work on one queue regardless. Treat a green
/// `diff_gpu_probe` as evidence about the *shaders*, never as evidence about
/// this barrier set. See task-3-report.md.
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
/// ### Caller-owned buffers the loop must also order (`extraBarrierBuffers`)
///
/// `WavefrontBuffers` is not the whole of what a bounce writes. `record()`
/// therefore takes an optional `std::span<const VkBuffer>` of extra,
/// CALLER-OWNED buffers and folds each one into the two whole-buffer
/// COMPUTE -> COMPUTE barriers that already cover state/queue/counter: the
/// post-generate barrier (barrier A) and the post-dispatch barrier (7) after
/// every compacting dispatch. Nothing else changes -- the counter-only fill
/// barriers (1)/(3) and the COMPUTE -> DRAW_INDIRECT barrier (5) stay
/// counter-only, because the extras are neither filled nor read as indirect
/// commands.
///
/// This exists because barrier (7) supplies an *execution* dependency for
/// every buffer but restricts availability/visibility to the buffers named
/// in its `VkBufferMemoryBarrier` list -- the exact distinction spelled out
/// at (7)'s KNOWN GAP note in the .cpp. `wf_scatter.comp` writes its
/// `debugDraws` (u1, u2, drawCount) triple at a FIXED `pathIndex*3` offset on
/// EVERY bounce, so in a fused run of `bounces >= 2` several scatter
/// dispatches write the same bytes inside one command buffer, and without
/// the extras mechanism nothing made bounce k's write available before
/// bounce k+1 overwrote it. The probe's per-bounce RNG check depends on the
/// LAST bounce's write being the survivor; it passed only because the driver
/// serialises. Passing `debugDraws` here is what actually orders it.
///
/// The parameter is a `std::span` passed per call rather than a setter
/// storing a list on the loop, for two reasons. A stored span would outlive
/// the array it points at as soon as one caller built it as a temporary --
/// a dangling-view class of bug this class has no way to detect -- whereas a
/// parameter cannot outlive the call. And which buffers a given `record()`
/// must order is a property of THAT recording (a caller with a debug sink in
/// one pass and a film accumulator in another passes different lists), not
/// configuration of the loop, so it belongs with the other per-call
/// arguments next to `cmd` and `bounces`. Defaulted to `{}`, so every
/// existing caller compiles and emits byte-identical commands.
///
/// Stage 0b-2b (the integrator port: BSDF, NEE, MIS, environment sampling)
/// added more scatter-side outputs living outside `WavefrontBuffers`.
/// `VK_NULL_HANDLE` entries are skipped, so a caller may pass a fixed-size
/// array with optional slots. Task 3's environment-sample sink (binding 6 of
/// `wf_scatter.comp`, written at a fixed `pathIndex*4` offset every bounce)
/// is the first of them and belongs here for exactly `debugDraws`' reason;
/// Task 4's next-event sink (binding 7, a fixed `pathIndex*25` offset every
/// bounce) is the second, for the identical reason. Task 4 also gave
/// `wf_scatter.comp` an acceleration structure at binding 8 for its shadow
/// rays -- that one does NOT belong here, on the read-only rule above: no
/// dispatch writes an acceleration structure.
///
/// **Task 5's FILM (binding 9) is the third, and it is the one where the
/// consequence of forgetting is worst.** The other two are probe-only sinks
/// whose stale values a check might notice; the film is the actual image,
/// it is written at a fixed `pixelIndex*3` offset on EVERY bounce, and the
/// write is an `atomicAdd` -- a read-modify-write, so a missing barrier
/// costs the *read* of bounce k-1's accumulated value as well as the
/// write-after-write. A caller that allocates a film and does not pass it
/// here has an integrator that silently drops radiance, and nothing in this
/// repository will say so. See `wf_scatter.comp`'s note at the atomicAdd.
///
/// READ-ONLY buffers do NOT belong here. Task 3 also added the two
/// environment CDF buffers at bindings 4 and 5, and they are deliberately
/// absent from every caller's extras list: no dispatch writes them, so there
/// is no write to make available and nothing to order. Adding them would not
/// be harmless noise -- it would blur the one rule this parameter encodes,
/// which is that it names buffers the recorded dispatches WRITE.
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
        /// The surface's base colour, as a grey scalar. `wf_scatter.comp`
        /// expands it to vec3(albedo) and feeds it to
        /// `shaders/includes/diff/bsdf.glsl`'s Lambert + GGX BSDF as the
        /// base colour; with the default material below (pure Lambertian)
        /// the resulting per-bounce estimator weight f*cos/pdf is exactly
        /// this value, which is why it is still spelled "albedo".
        float albedo{0.5f};
        /// GGX roughness, passed through `unpackHitPbr` (which floors it at
        /// 0.01) inside the shader. Irrelevant while `specularWeight` is 0.
        float roughness{1.0f};
        /// Metalness. 0 = dielectric (diffuse lobe carries the base colour),
        /// 1 = conductor (no diffuse lobe, specular tinted by the base
        /// colour, lobe selection forced to specular).
        float metallic{0.0f};
        /// Scales the DIELECTRIC specular lobe -- both its contribution to
        /// f and its share of the lobe-selection probability. 0 removes the
        /// specular lobe entirely, leaving a pure Lambertian surface whose
        /// estimator weight is exactly `albedo`; 1 reproduces the RT
        /// pipeline's F0 = 0.04 dielectric. Conductors (`metallic` = 1) are
        /// unaffected -- a metal has no diffuse lobe to fall back to.
        ///
        /// It defaults to 0, not 1, so that every pre-existing caller keeps
        /// producing the exact constant-albedo throughput decay checks 14
        /// and 17 assert bit-exactly.
        float specularWeight{0.0f};
        /// Number of PIXELS in the caller-owned film buffer bound at
        /// `wf_scatter.comp`'s binding 9 (the buffer holds 3 floats per
        /// pixel). 0 disables film accumulation entirely.
        ///
        /// This is in Config and not derived from `buffers` -- unlike
        /// `capacity` and the environment tail -- because the film is
        /// CALLER-OWNED. `WavefrontBuffers` does not allocate it and cannot
        /// state its size; the only object that can is the caller that
        /// allocated it and bound it into the scatter stage's descriptor
        /// set.
        ///
        /// The shader's bounds guard checks a path's pixelIndex against THIS
        /// number, not against the film buffer's real byte size -- it has no
        /// other way to learn that size. So the guard rejects a pixelIndex
        /// outside the range this value states, but it CANNOT catch this
        /// value itself being wrong: a caller that passes a filmPixelCount
        /// larger than the buffer it actually bound still gets a write past
        /// the end of that allocation, guard and all. Keeping this number
        /// and the buffer's real size in agreement is the caller's own
        /// invariant, not something the shader verifies for it.
        std::uint32_t filmPixelCount{0};
        /// Total number of floats in the CALLER-OWNED gradient arena bound at
        /// the traversal's binding 10, and the FLOAT index at which the
        /// base-colour parameter's gradient block begins inside it.
        ///
        /// `gradArenaFloats == 0` disables every gradient write, which is
        /// what every caller that binds a placeholder buffer there gets --
        /// and is the default, so no existing caller changes behaviour.
        ///
        /// These are here and not derived from `buffers` for
        /// `filmPixelCount`'s reason exactly: `GradientArena` is caller-owned
        /// and `WavefrontBuffers` cannot state its size. A caller that passes
        /// a count larger than the buffer it actually bound still writes past
        /// the end of that allocation, guard and all.
        ///
        /// A caller setting these MUST also pass the arena's VkBuffer to
        /// `record`'s `extraBarrierBuffers`: the write is an `atomicAdd`, so
        /// bounce k's accumulation must be available to bounce k+1's, and
        /// a `VkBufferMemoryBarrier`'s memory scope covers only the buffers
        /// it names.
        std::uint32_t gradArenaFloats{0};
        std::uint32_t gradAlbedoOffset{0};
        /// THE DETACHED-SAMPLING MATERIAL (Stage 1 Task 3). The material every
        /// SAMPLING DECISION inside the traversal is made from -- the lobe
        /// choice and the GGX VNDF's alpha -- as opposed to the four fields
        /// above, which are what `f` and the densities are EVALUATED with.
        ///
        /// A NON-POSITIVE `samplingRoughness` means "use the evaluated
        /// material", which is the default and what every renderer wants.
        /// The sentinel is resolved IN THE TRAVERSAL (`pc.sampleRoughness >
        /// 0.0`, not `>= 0.0` -- see traverse.glsl), not here: this member's
        /// `-1.0f` default member initialiser is what makes a ScatterPush
        /// built by hand somewhere else -- several probes do -- land on
        /// "no override" rather than a silently cosine-sampled dispatch, but
        /// it is the SHADER-SIDE `> 0.0` test that actually closes the hole,
        /// because it is what makes a zero-filled tail (this member never
        /// having been set at all, its NSDMI stripped by a braced list that
        /// stops early) read the same way as this member's own explicit
        /// -1.0f default: as "no override". This member's default initialiser
        /// alone is a host-side mechanism and is NOT sufficient by itself --
        /// see traverse.glsl's comment on the same sentinel for why the
        /// shader-side test is what is actually load-bearing. With the
        /// override active the shader's split body reduces, expression for
        /// expression, to the single-material one it replaced. Nothing that
        /// predates this task changes behaviour.
        ///
        /// A CALLER CAN HALF-SET THE OVERRIDE: setting only
        /// `samplingRoughness` (to a positive value) while leaving
        /// `samplingAlbedo`/`samplingMetallic`/`samplingSpecularWeight` at
        /// their own defaults activates the override with those three
        /// implicitly zero, which is a legitimate but easy-to-miss partial
        /// configuration -- there is no compile-time or run-time check that
        /// all four were set together.
        ///
        /// Setting it is a MEASUREMENT, not a rendering mode. Spec section 6.3
        /// does not differentiate sampled directions, so the derivative this
        /// subsystem computes is the estimator's derivative at FIXED
        /// directions; a finite difference that perturbs roughness or metallic
        /// and re-runs the sampler also moves every direction, and measures
        /// the sum of the term the adjoint computes and a term it deliberately
        /// omits. Freezing this material at theta_0 across the +h and -h
        /// renders holds the whole path still, so the difference quotient
        /// measures what the adjoint computes and nothing else. With the
        /// override set, the film is not an unbiased estimator of anything.
        float samplingAlbedo{0.0f};
        float samplingRoughness{-1.0f};
        float samplingMetallic{0.0f};
        float samplingSpecularWeight{0.0f};
        /// Which scalar parameter the REPLAY hook differentiates, and whether
        /// the traversal maintains the forward-mode throughput tangent in path
        /// state. 0 = base colour (Stage 1 Task 2's closed-form path, and the
        /// default, so no existing caller changes), 1 = roughness,
        /// 2 = metallic. The enumerators are DIFF_PARAM_* in
        /// shaders/includes/diff/bsdf_adjoint.glsl.
        std::uint32_t diffParam{0};
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

    /// Matches `shaders/diff/wf_scatter.comp`'s Push block. This has grown a
    /// tail field in Tasks 2, 3 and 5, each time by hand-editing both this
    /// struct and the shader's Push block to match -- the same
    /// naming-each-other-in-a-comment situation `kNeeSampleFloats` was in
    /// before it got a runtime tie. `tests/diff/diff_gpu_probe.cpp`'s
    /// `checkScatterPushSizeTie()` closes that gap the same way: it parses
    /// wf_scatter.comp's Push block out of the SOURCE and fails the whole
    /// probe if `sizeof(ScatterPush)` disagrees with it. A struct member
    /// added here without a matching shader field (or vice versa) is
    /// therefore a build-time-adjacent failure, not a silent wrong-field
    /// push at every offset after the point of drift.
    struct ScatterPush {
        std::uint32_t capacity;
        std::uint32_t srcQueueBase;
        std::uint32_t srcCountSlot;
        std::uint32_t dstQueueBase;
        std::uint32_t dstCountSlot;
        float albedo;
        std::uint32_t iterationSeed;
        // --- Material (Stage 0b-2b Task 2). See Config above for meaning.
        float roughness;
        float metallic;
        float specularWeight;
        // --- Environment (Stage 0b-2b Task 3). NOT in Config: these are
        // properties of the WavefrontBuffers passed to record(), which is
        // the only object that can state them authoritatively -- the same
        // reason `capacity` is not in Config either. record() fills all
        // three from `buffers`.
        std::uint32_t envWidth;
        std::uint32_t envHeight;
        float envIntegral;
        // --- Film (Stage 0b-2b Task 5). Comes from Config, NOT from
        // `buffers`, for the reason Config::filmPixelCount gives: the film
        // is caller-owned and WavefrontBuffers cannot state its size.
        std::uint32_t filmPixelCount;
        // --- Gradient arena (Stage 1 Task 2). From Config, for the film's
        // reason: the arena is caller-owned.
        std::uint32_t gradArenaFloats;
        std::uint32_t gradAlbedoOffset;
        // --- The detached-sampling material and the differentiated parameter
        // (Stage 1 Task 3). From Config, passed through verbatim.
        //
        // THESE FIVE CARRY DEFAULT MEMBER INITIALISERS AND THE ONES ABOVE DO
        // NOT, on purpose. Several probes build a ScatterPush by hand with a
        // braced list that stops at `filmPixelCount`; every member after the
        // last initialiser is then value-initialised to zero, which was
        // harmless while zero meant "no gradient arena" but is NOT harmless
        // for a sampling material -- a zeroed one is roughness 0 (floored to
        // 0.01), metallic 0, specularWeight 0, i.e. a lobe probability of
        // exactly 0, which turns every dispatch into a cosine-hemisphere
        // sampler while `f` and the density stay GGX. That is a silently
        // biased estimator, and it is what the mixture furnace check caught
        // the first time this tail was added without a sentinel.
        //
        // So `sampleRoughness` defaults to -1, and the TRAVERSAL reads any
        // NON-POSITIVE value (`pc.sampleRoughness > 0.0`, not `>= 0.0` -- see
        // traverse.glsl) as "sample with the evaluated material". Testing
        // `> 0.0` rather than `>= 0.0` is what actually closes the hole: a
        // push built anywhere else that leaves this whole tail zero-filled
        // gets `sampleRoughness == 0.0`, which `> 0.0` reads the same way as
        // this struct's own `-1.0f` default -- "no override" -- at no cost,
        // because `unpackHitPbr` floors roughness at 0.01 regardless, so no
        // caller can ever observe a sampling roughness of exactly 0.0 as
        // distinct from 0.01. The resolution therefore has to live in the
        // shader, not in record(): this struct's default member initialiser
        // only helps a caller that goes through record() at all, and the
        // several probes this comment refers to build a ScatterPush by hand
        // and never call record().
        float sampleAlbedo{0.0f};
        float sampleRoughness{-1.0f};
        float sampleMetallic{0.0f};
        float sampleSpecularWeight{0.0f};
        std::uint32_t diffParam{0};
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
    ///
    /// `extraBarrierBuffers` names caller-owned buffers the recorded
    /// dispatches also write and that the loop must therefore order against
    /// itself -- see "Caller-owned buffers the loop must also order" above
    /// for what it does, why it is a per-call span, and why omitting it for
    /// `debugDraws` was a real missing memory dependency rather than a
    /// tidiness issue. Defaulting it to `{}` reproduces the previous
    /// behaviour exactly.
    void record(VkCommandBuffer cmd, WavefrontBuffers& buffers, std::uint32_t bounces,
                std::span<const VkBuffer> extraBarrierBuffers = {}) const;

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
    ///
    /// `extraBarrierBuffers` is forwarded from `record()` and folded into
    /// the post-dispatch barrier (7) only.
    void recordCompactingStage(VkCommandBuffer cmd, const WavefrontBuffers& buffers,
                               WavefrontStage& stage, const Ring& src, const Ring& dst,
                               std::span<const VkBuffer> extraBarrierBuffers) const;

    Config m_config{};
    WavefrontStage* m_generate{nullptr};
    WavefrontStage* m_prepareIndirect{nullptr};
    WavefrontStage* m_intersect{nullptr};
    WavefrontStage* m_scatter{nullptr};
};

}  // namespace ohao::diff
