// RECORDING one differentiable render, without submitting it. Stage 5,
// item 1: the fifth and last slice of the orchestration move.
//
// THIS IS THE SHAPE THE PROBE DID NOT HAVE, and the difference is the whole
// point. `GpuProbeContext::runWavefrontGradientProbe` submits inside the
// call and reads the film back before returning -- which is what a test
// wants and the opposite of what an engine can use. An engine has one
// command buffer per frame, records many passes into it, and submits once; a
// library that submits for you cannot be put in that frame at all.
//
// So the function below RECORDS and returns. What to do with the command
// buffer afterwards -- submit it, wait on it, read anything back -- is the
// caller's, and `recordHostReadBarrier` is here for the callers that do.
#pragma once

#include "diff/grad/gradient_arena.hpp"
#include "diff/wavefront/gradient_frame.hpp"
#include "diff/wavefront/gradient_stages.hpp"
#include "diff/wavefront/scatter_sinks.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"

#include <vulkan/vulkan.h>

#include <span>

namespace ohao::diff {

/// Everything ONE gradient run dispatches against that is not the gradient
/// arena. All of it is the CALLER'S: an engine already has a wavefront's
/// buffers, five compute pipelines, an acceleration structure and a vertex
/// buffer, and is not going to hand them over per frame.
struct GradientResources {
    WavefrontBuffers* buffers{nullptr};
    GradientStages* stages{nullptr};
    ScatterSinks* sinks{nullptr};
    GradientStages::Scene scene{};
    /// Binding 11's primal, read by both instantiations. May be a one-float
    /// placeholder when no emission texture is configured -- but it must be
    /// a VALID buffer either way, because a statically-used binding needs a
    /// descriptor even when the branch that reads it never runs.
    VkBuffer emissionTexture{VK_NULL_HANDLE};
    /// Binding 12's dL/dpixel. Same placeholder rule.
    VkBuffer adjointSeed{VK_NULL_HANDLE};
    /// Binding 13's spec-10.2 sensitivity map. Same placeholder rule, and
    /// the same reason: the binding is statically used by the one traversal
    /// source, so both instantiations need a descriptor for it even though
    /// the forward one is pushed sensitivityFloats = 0 and never writes it.
    VkBuffer sensitivity{VK_NULL_HANDLE};
    /// Binding 14's environment RADIANCE image. Same placeholder rule.
    VkBuffer envImage{VK_NULL_HANDLE};
    [[nodiscard]] bool valid() const noexcept;
};

/// RECORDS one instantiation -- forward or replay -- into `cmd`. Does not
/// submit, does not wait, does not read anything back.
///
/// WHAT IS RECORDED, in order: the wavefront's own buffers are zeroed; this
/// run's film is zeroed with its own TRANSFER_WRITE ->
/// SHADER_READ|SHADER_WRITE barrier (the film is caller-owned and
/// read-modify-written, so the wavefront's zero does not cover it); the
/// gradient arena is zeroed unless `zeroArena` is false; then the bounce
/// loop.
///
/// `zeroArena` FALSE IS THE MULTI-VIEW BATCH. The gradients of several views
/// of one scene sum, so a caller accumulating them must not clear between.
/// The barrier that `GradientArena::zero` also records is then not recorded
/// either, which is correct rather than an oversight: there is no
/// TRANSFER_WRITE to order against, and the atomicAdd is read-modify-write
/// on the same queue.
///
/// DESCRIPTORS MUST ALREADY BE WRITTEN. Call `GradientStages::bindAll`
/// before recording -- once, covering both instantiations -- because
/// `vkUpdateDescriptorSets` may not run while a command buffer using those
/// sets is pending, and recording both runs into one buffer makes that the
/// normal case rather than an edge one.
///
/// Returns false WITHOUT RECORDING ANYTHING if the resources are incomplete,
/// the pipelines are not built, the frame's film and the wavefront disagree
/// about how many pixels there are, or the two instantiations' loop
/// configurations disagree on a field that steers the traversal.
[[nodiscard]] bool recordGradientRun(VkCommandBuffer cmd, GradientRun run,
                                     const GradientFrame& frame,
                                     const GradientResources& resources, GradientArena& arena,
                                     bool zeroArena = true);

/// RECORDS the COMPUTE -> HOST barrier a caller needs before mapping any of
/// `buffers`. Does not submit.
///
/// SEPARATE FROM recordGradientRun BECAUSE IT IS THE CALLER'S QUESTION, not
/// the run's: an engine that steps Adam on the GPU reads nothing back and
/// wants no host barrier at all, while a test reads the film every
/// iteration. Each buffer the host will map must be NAMED here --
/// vkQueueWaitIdle does not make writes visible in the host domain, and a
/// VkBufferMemoryBarrier's memory scope covers only the buffers it lists.
/// Null handles in the span are skipped, so a caller with an optional
/// readback can size its array once.
void recordHostReadBarrier(VkCommandBuffer cmd, std::span<const VkBuffer> buffers);

}  // namespace ohao::diff
