// The recording the header argues for. Moved out of
// tests/diff/context/probes_gradient.cpp's runImmediate lambda, minus the
// submit and the readback that surrounded it.
#include "diff/wavefront/gradient_render.hpp"

#include "diff/wavefront/wavefront_loop.hpp"

#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GradientResources::valid() const noexcept {
    return buffers != nullptr && stages != nullptr && sinks != nullptr && scene.valid() &&
           emissionTexture != VK_NULL_HANDLE && adjointSeed != VK_NULL_HANDLE &&
           sensitivity != VK_NULL_HANDLE;
}

void recordHostReadBarrier(VkCommandBuffer cmd, std::span<const VkBuffer> buffers) {
    if (cmd == VK_NULL_HANDLE || buffers.empty()) return;
    std::vector<VkBufferMemoryBarrier> toHost;
    toHost.reserve(buffers.size());
    for (VkBuffer buffer : buffers) {
        if (buffer == VK_NULL_HANDLE) continue;
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        toHost.push_back(barrier);
    }
    if (toHost.empty()) return;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, static_cast<std::uint32_t>(toHost.size()), toHost.data(), 0,
                         nullptr);
}

bool recordGradientRun(VkCommandBuffer cmd, GradientRun run, const GradientFrame& frame,
                       const GradientResources& resources, GradientArena& arena, bool zeroArena) {
    if (cmd == VK_NULL_HANDLE || !resources.valid()) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: a null command buffer or an incomplete "
                     "GradientResources. Every one of buffers, stages, sinks, the scene's three "
                     "handles, the emission-texture primal and the adjoint seed must be present "
                     "-- a statically-used binding needs a descriptor even when the branch that "
                     "reads it never runs\n");
        return false;
    }
    if (!resources.stages->built()) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: GradientStages::build has not run, so "
                     "there are no pipelines to record\n");
        return false;
    }
    if (arena.buffer() == VK_NULL_HANDLE) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: the gradient arena has no buffer. Both "
                     "instantiations bind it at binding 10 -- the forward one too, which never "
                     "writes it -- so there is nothing to record against\n");
        return false;
    }
    if (frame.width == 0u || frame.height != kGenerateLocalY ||
        (frame.width % kGenerateLocalX) != 0u || frame.bounces == 0u) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: needs height == %u (wf_generate.comp "
                     "dispatches (groupCountX, 1, 1) and covers that many rows per group), a "
                     "non-zero width that is a multiple of %u, and bounces > 0; got %ux%u, "
                     "bounces %u\n",
                     kGenerateLocalY, kGenerateLocalX, frame.width, frame.height, frame.bounces);
        return false;
    }
    // THE FILM AND THE WAVEFRONT MUST AGREE ABOUT THE PIXEL COUNT. The film
    // is indexed by pixel index, which is the path index for a one-sample-
    // per-pixel render; a frame claiming more pixels than the wavefront has
    // capacity for would have the traversal write past the end of a
    // caller-owned buffer, and that is not a validation error, because the
    // binding's range is the whole buffer.
    const std::uint32_t capacity = resources.buffers->layout().capacity();
    if (frame.width * frame.height != capacity || frame.filmPixelCount > capacity) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: the frame is %ux%u with a film of %u "
                     "pixels, against a wavefront capacity of %u. width*height must equal the "
                     "capacity -- that is also the one-sample-per-pixel condition -- and the film "
                     "may not claim more pixels than there are paths\n",
                     frame.width, frame.height, frame.filmPixelCount, capacity);
        return false;
    }

    const bool isReplay = (run == GradientRun::Replay);
    ScatterSinkSet& sinkSet = isReplay ? resources.sinks->replay() : resources.sinks->forward();

    // --- The stages, configured. The push constants are gradient_frame's;
    // WHICH stage object is the scatter is what makes this the forward or the
    // replay instantiation.
    WavefrontStage& generate = resources.stages->generate();
    const GenerateCameraPush genPush = generatePush(frame, capacity);
    generate.setPushConstants(&genPush, sizeof(genPush));
    generate.setGroupCount(WavefrontStage::Fixed{generateGroupCountX(frame.width)});

    WavefrontLoop loop;
    loop.setGenerate(generate);
    loop.setPrepareIndirect(resources.stages->prepareIndirect());
    loop.setIntersect(resources.stages->intersect());
    loop.setScatter(resources.stages->scatter(isReplay ? 1u : 0u));

    // BOTH configurations are built, not just this run's, so that the
    // steering invariant can be checked HERE and not only in the unit tests.
    // This is where a future edit would introduce the split: a field pushed
    // to one run and not the other makes the replay run the derivative of a
    // path the forward run did not take, and every check that compares the
    // two against each other would still pass, because both sides move.
    const WavefrontLoop::Config forwardConfig = loopConfigFor(frame, GradientRun::Forward);
    const WavefrontLoop::Config replayConfig = loopConfigFor(frame, GradientRun::Replay);
    if (!steeringFieldsAgree(forwardConfig, replayConfig)) {
        std::fprintf(stderr,
                     "[diff] recordGradientRun refused: the forward and replay loop "
                     "configurations disagree on a field that STEERS THE TRAVERSAL, so the two "
                     "instantiations would not walk one path. See loopConfigFor in "
                     "ohao/diff/wavefront/gradient_frame.cpp\n");
        return false;
    }
    loop.setConfig(isReplay ? replayConfig : forwardConfig);

    // --- Recording proper. Nothing below submits or waits.
    resources.buffers->zero(cmd);

    // The film is CALLER-OWNED and read-modify-written, so it is not covered
    // by the wavefront's own zero and gets its own fill plus a
    // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier.
    vkCmdFillBuffer(cmd, sinkSet.film.buffer, 0, VK_WHOLE_SIZE, 0u);
    VkBufferMemoryBarrier filmZero{};
    filmZero.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    filmZero.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    filmZero.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    filmZero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    filmZero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    filmZero.buffer = sinkSet.film.buffer;
    filmZero.offset = 0;
    filmZero.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &filmZero, 0, nullptr);

    // THE SENSITIVITY MAP IS ZEROED TOO, for the film's reason: it is
    // caller-owned and read-modify-written by atomicAdd, so a map left from a
    // previous run would be added to rather than replaced. Only when one is
    // requested -- the placeholder buffer a caller binds when it wants no map
    // may be a single float, and filling it would be a write to a buffer
    // nothing reads.
    //
    // NOT GATED ON `zeroArena`. Accumulating gradients across views is a
    // deliberate multi-view sum into ONE parameter slot; a sensitivity map is
    // an image of one view, and silently summing several views' images
    // together would be a different picture with no name. A caller that wants
    // an accumulated map can bind the same buffer and say so by passing a
    // frame with no map on the runs it does not want counted.
    if (frame.sensitivityFloats > 0u) {
        vkCmdFillBuffer(cmd, resources.sensitivity, 0, VK_WHOLE_SIZE, 0u);
        VkBufferMemoryBarrier mapZero = filmZero;
        mapZero.buffer = resources.sensitivity;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &mapZero, 0,
                             nullptr);
    }

    // The arena is zeroed on BOTH runs, in the same command buffer as the
    // loop that follows -- which is exactly the configuration
    // GradientArena::zero's own comment says its barrier becomes
    // load-bearing in. Skipped for a multi-view batch; see the header.
    if (zeroArena) arena.zero(cmd);

    const VkBuffer loopExtras[5] = {sinkSet.trace.buffer, sinkSet.env.buffer, sinkSet.nee.buffer,
                                    sinkSet.film.buffer, arena.buffer()};
    loop.record(cmd, *resources.buffers, frame.bounces, loopExtras);
    return true;
}

}  // namespace ohao::diff
