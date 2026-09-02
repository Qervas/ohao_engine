// The boundary probe (Stage 3): dispatch boundary_sample.comp over an edge
// list and a screen-space vertex array, returning dJ/dv for every vertex.
//
// Separate from the wavefront entry points, like the loss and Adam probes:
// this kernel binds no path state, no queue and no acceleration structure,
// and that is the structural half of spec 4.1's claim that the boundary term
// is a SEPARATE dispatch summed into the same arena rather than a branch
// inside the interior one.
#include "gpu_probe_context.hpp"

#include "diff/grad/gradient_arena.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GpuProbeContext::runBoundaryProbe(const std::vector<float>& screenPositions,
                                       const std::vector<std::uint32_t>& edgeVertexPairs,
                                       std::uint32_t imageWidth, std::uint32_t imageHeight,
                                       const BoundaryRadiance& radiance,
                                       const std::vector<std::uint32_t>& silhouetteFlags,
                                       const std::vector<float>& adjointSeed,
                                       ohao::diff::GradientArena* arena,
                                       std::size_t arenaBlock, std::uint32_t arenaFloatOffset,
                                       std::vector<float>& outVertexGradients) {
    outVertexGradients.clear();
    if (screenPositions.empty() || screenPositions.size() % 2u != 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: screenPositions must be a "
                              "non-zero multiple of 2 floats (x, y per vertex)\n");
        return false;
    }
    if (edgeVertexPairs.empty() || edgeVertexPairs.size() % 2u != 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: edgeVertexPairs must be a "
                              "non-zero multiple of 2 uints\n");
        return false;
    }
    if (imageWidth == 0u || imageHeight == 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: a zero-sized image has no "
                              "pixels for an edge to cross\n");
        return false;
    }
    const auto vertexCount = static_cast<std::uint32_t>(screenPositions.size() / 2u);
    const auto edgeCount = static_cast<std::uint32_t>(edgeVertexPairs.size() / 2u);
    for (std::uint32_t i : edgeVertexPairs) {
        if (i >= vertexCount) {
            std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: an edge names vertex %u of "
                                  "%u -- the shader guards this but the caller should not rely "
                                  "on it\n",
                         i, vertexCount);
            return false;
        }
    }

    // An EMPTY flag array means "every edge", which the single-triangle
    // coverage case wants: a lone triangle against a background has a real
    // jump across all three edges and no silhouette pass is involved. A
    // non-empty one must cover every edge, or the shader would read past it.
    const bool useFlags = !silhouetteFlags.empty();
    if (useFlags && silhouetteFlags.size() != edgeCount) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runBoundaryProbe: %zu silhouette flags for %u edges. "
                     "They must correspond one to one, or the filter reads past its end\n",
                     silhouetteFlags.size(), edgeCount);
        return false;
    }
    const std::vector<std::uint32_t> allOnes(edgeCount, 1u);
    GpuBuffer flagBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        useFlags ? std::span<const std::uint32_t>(silhouetteFlags)
                 : std::span<const std::uint32_t>(allOnes),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // An EMPTY seed means w = 1 everywhere -- the sum-of-image objective,
    // which is what every boundary check written before this one measures.
    // A non-empty one must cover every pixel.
    const bool useSeed = !adjointSeed.empty();
    const std::size_t pixelCount =
        static_cast<std::size_t>(imageWidth) * static_cast<std::size_t>(imageHeight);
    if (useSeed && adjointSeed.size() != pixelCount) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runBoundaryProbe: adjointSeed holds %zu floats for a "
                     "%ux%u image, which needs exactly %zu -- one per pixel\n",
                     adjointSeed.size(), imageWidth, imageHeight, pixelCount);
        return false;
    }
    const std::vector<float> seedOnes(pixelCount, 1.0f);
    GpuBuffer seedBuffer = m_allocator.createBufferFromSpan<float>(
        useSeed ? std::span<const float>(adjointSeed) : std::span<const float>(seedOnes),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // THE ARENA, WHEN THE CALLER HAS ONE. Without it this pass allocates a
    // buffer of its own, which is what the checks written before the arena
    // existed use. With it, the gradient lands in a registered parameter's
    // block beside the interior term's -- spec 4.1's "summed into the same
    // arena".
    const bool intoArena = arena != nullptr;
    if (intoArena && arena->buffer() == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: an arena was supplied but is "
                              "not built\n");
        return false;
    }
    const std::vector<float> zeroGrad(screenPositions.size(), 0.0f);
    GpuBuffer posBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(screenPositions), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer edgeBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(edgeVertexPairs), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer gradBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(zeroGrad), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    bool ok = posBuffer.isValid() && edgeBuffer.isValid() && gradBuffer.isValid() &&
              flagBuffer.isValid() && seedBuffer.isValid();
    if (!ok) std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: allocation failed\n");

    struct BoundaryPush {
        std::uint32_t edgeCount;
        std::uint32_t imageWidth;
        std::uint32_t imageHeight;
        std::uint32_t vertexCount;
        float lIn;
        float lOut;
        // Two vec2s in the shader. They sit at byte offsets 24 and 32, both
        // multiples of 8, so the 4-byte-aligned C++ members below land where
        // GLSL's 8-byte vec2 alignment puts them. Inserting a float above
        // this point would silently break that.
        float gradIn[2];
        float gradOut[2];
        std::uint32_t useFlags;
        std::uint32_t useSeed;
        std::uint32_t gradOffset;
    } push{edgeCount,
           imageWidth,
           imageHeight,
           vertexCount,
           radiance.lIn,
           radiance.lOut,
           {radiance.gradIn[0], radiance.gradIn[1]},
           {radiance.gradOut[0], radiance.gradOut[1]},
           useFlags ? 1u : 0u,
           useSeed ? 1u : 0u,
           intoArena ? arenaFloatOffset : 0u};

    WavefrontStage stage;
    if (ok) {
        const VkDescriptorType bindings[5] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        ok = stage.build(m_device, "diff_boundary_sample.comp.spv", bindings,
                         sizeof(BoundaryPush));
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: build failed\n");
    }
    if (ok) {
        const VkBuffer buffers[5] = {posBuffer.buffer, edgeBuffer.buffer,
                                     intoArena ? arena->buffer() : gradBuffer.buffer,
                                     flagBuffer.buffer, seedBuffer.buffer};
        ok = stage.bindBuffers(m_device, buffers);
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: bindBuffers failed\n");
    }
    if (ok) {
        // ONE INVOCATION PER (EDGE, PIXEL). Simple rather than clever: an
        // edge crosses a handful of the pixels this launches for it, and the
        // rest return on the clip. A bounding-box dispatch is the obvious
        // optimisation and is deliberately not here -- it would need its own
        // correctness argument, and this pass has to be right before it is
        // fast.
        const std::uint64_t total =
            static_cast<std::uint64_t>(edgeCount) * imageWidth * imageHeight;
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(
            WavefrontStage::Fixed{static_cast<std::uint32_t>((total + 63u) / 64u)});
        runImmediate([&](VkCommandBuffer cmd) {
            // ZERO THE ARENA FIRST, in the same command buffer.
            //
            // GradientArena::build does NOT zero, and this pass scatters with
            // atomicAdd -- so without this it accumulates onto whatever the
            // allocator last left in that memory. It was not zeros: the first
            // run of check 61 read 1.69988 out of float 0, which is the
            // triangle's own 1.7 coordinate recycled from a destroyed
            // position buffer. That is the worst possible shape for such a
            // bug, because a stale POSITION is numerically plausible as a
            // gradient.
            //
            // The interior path has always done this (probes_gradient.cpp),
            // and zero() records its own TRANSFER_WRITE -> SHADER_READ|WRITE
            // barrier, which is exactly what makes the dispatch below safe to
            // follow it.
            if (intoArena) arena->zero(cmd);
            stage.record(cmd);
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = intoArena ? arena->buffer() : gradBuffer.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        });
        if (intoArena) {
            // Read the parameter's own block back, not the whole arena: the
            // caller asked for this parameter's gradient, and the arena's
            // other blocks are another parameter's business.
            outVertexGradients = arena->readback(m_allocator, arenaBlock);
            if (outVertexGradients.size() != screenPositions.size()) {
                std::fprintf(stderr,
                             "[GpuProbeContext] runBoundaryProbe: the arena block read back %zu "
                             "floats for %zu vertex components\n",
                             outVertexGradients.size(), screenPositions.size());
                ok = false;
            }
        } else {
            const auto* mapped = static_cast<const float*>(gradBuffer.getMappedData());
            if (mapped == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: the gradient buffer is "
                                      "not host mapped\n");
                ok = false;
            } else {
                outVertexGradients.assign(mapped, mapped + screenPositions.size());
            }
        }
    }

    stage.destroy(m_device);
    if (seedBuffer.isValid()) m_allocator.destroyBuffer(seedBuffer);
    if (flagBuffer.isValid()) m_allocator.destroyBuffer(flagBuffer);
    if (gradBuffer.isValid()) m_allocator.destroyBuffer(gradBuffer);
    if (edgeBuffer.isValid()) m_allocator.destroyBuffer(edgeBuffer);
    if (posBuffer.isValid()) m_allocator.destroyBuffer(posBuffer);
    return ok;
}

}  // namespace ohao::diff
