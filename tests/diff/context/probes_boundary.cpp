// The boundary probe (Stage 3): dispatch boundary_sample.comp over an edge
// list and a screen-space vertex array, returning dJ/dv for every vertex.
//
// Separate from the wavefront entry points, like the loss and Adam probes:
// this kernel binds no path state, no queue and no acceleration structure,
// and that is the structural half of spec 4.1's claim that the boundary term
// is a SEPARATE dispatch summed into the same arena rather than a branch
// inside the interior one.
#include "gpu_probe_context.hpp"

#include "diff/wavefront/wavefront_stage.hpp"

#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GpuProbeContext::runBoundaryProbe(const std::vector<float>& screenPositions,
                                       const std::vector<std::uint32_t>& edgeVertexPairs,
                                       std::uint32_t imageWidth, std::uint32_t imageHeight,
                                       float lIn, float lOut,
                                       const std::vector<std::uint32_t>& silhouetteFlags,
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

    const std::vector<float> zeroGrad(screenPositions.size(), 0.0f);
    GpuBuffer posBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(screenPositions), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer edgeBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(edgeVertexPairs), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer gradBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(zeroGrad), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    bool ok = posBuffer.isValid() && edgeBuffer.isValid() && gradBuffer.isValid() &&
              flagBuffer.isValid();
    if (!ok) std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: allocation failed\n");

    struct BoundaryPush {
        std::uint32_t edgeCount;
        std::uint32_t imageWidth;
        std::uint32_t imageHeight;
        std::uint32_t vertexCount;
        float lIn;
        float lOut;
        std::uint32_t useFlags;
    } push{edgeCount, imageWidth, imageHeight, vertexCount, lIn, lOut, useFlags ? 1u : 0u};

    WavefrontStage stage;
    if (ok) {
        const VkDescriptorType bindings[4] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        ok = stage.build(m_device, "diff_boundary_sample.comp.spv", bindings,
                         sizeof(BoundaryPush));
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: build failed\n");
    }
    if (ok) {
        const VkBuffer buffers[4] = {posBuffer.buffer, edgeBuffer.buffer, gradBuffer.buffer,
                                     flagBuffer.buffer};
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
            stage.record(cmd);
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = gradBuffer.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        });
        const auto* mapped = static_cast<const float*>(gradBuffer.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runBoundaryProbe: the gradient buffer is not "
                                  "host mapped\n");
            ok = false;
        } else {
            outVertexGradients.assign(mapped, mapped + screenPositions.size());
        }
    }

    stage.destroy(m_device);
    if (flagBuffer.isValid()) m_allocator.destroyBuffer(flagBuffer);
    if (gradBuffer.isValid()) m_allocator.destroyBuffer(gradBuffer);
    if (edgeBuffer.isValid()) m_allocator.destroyBuffer(edgeBuffer);
    if (posBuffer.isValid()) m_allocator.destroyBuffer(posBuffer);
    return ok;
}

}  // namespace ohao::diff
