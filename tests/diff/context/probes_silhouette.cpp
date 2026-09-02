// The silhouette probe (Stage 3): dispatch silhouette_mark.comp over an edge
// record array, returning a 0/1 flag per edge and the marked count.
#include "gpu_probe_context.hpp"

#include "diff/wavefront/wavefront_stage.hpp"

#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GpuProbeContext::runSilhouetteProbe(const std::vector<float>& positions,
                                         const std::vector<std::uint32_t>& edgeRecords,
                                         const std::vector<std::uint32_t>& indices,
                                         const float cameraPos[3],
                                         std::vector<std::uint32_t>& outFlags,
                                         std::uint32_t& outCount) {
    outFlags.clear();
    outCount = 0u;
    if (positions.empty() || positions.size() % 3u != 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: positions must be a non-zero "
                              "multiple of 3 floats\n");
        return false;
    }
    if (edgeRecords.empty() || edgeRecords.size() % 4u != 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: edgeRecords must be a "
                              "non-zero multiple of 4 uints (v0, v1, face0, face1)\n");
        return false;
    }
    if (indices.empty() || indices.size() % 3u != 0u) {
        std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: indices must be a non-zero "
                              "multiple of 3\n");
        return false;
    }
    const auto edgeCount = static_cast<std::uint32_t>(edgeRecords.size() / 4u);
    const auto triangleCount = static_cast<std::uint32_t>(indices.size() / 3u);
    const auto vertexCount = static_cast<std::uint32_t>(positions.size() / 3u);

    const std::vector<std::uint32_t> zeroFlags(edgeCount, 0u);
    const std::vector<std::uint32_t> zeroCount{0u};
    GpuBuffer posBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer edgeBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(edgeRecords), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer triBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(indices), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer flagBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(zeroFlags), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer countBuffer = m_allocator.createBufferFromSpan<std::uint32_t>(
        std::span<const std::uint32_t>(zeroCount), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    bool ok = posBuffer.isValid() && edgeBuffer.isValid() && triBuffer.isValid() &&
              flagBuffer.isValid() && countBuffer.isValid();
    if (!ok) std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: allocation failed\n");

    struct SilhouettePush {
        std::uint32_t edgeCount;
        std::uint32_t triangleCount;
        std::uint32_t vertexCount;
        float cameraX;
        float cameraY;
        float cameraZ;
    } push{edgeCount, triangleCount, vertexCount, cameraPos[0], cameraPos[1], cameraPos[2]};

    WavefrontStage stage;
    if (ok) {
        const VkDescriptorType bindings[5] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        ok = stage.build(m_device, "diff_silhouette_mark.comp.spv", bindings,
                         sizeof(SilhouettePush));
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: build failed\n");
    }
    if (ok) {
        const VkBuffer buffers[5] = {posBuffer.buffer, edgeBuffer.buffer, triBuffer.buffer,
                                     flagBuffer.buffer, countBuffer.buffer};
        ok = stage.bindBuffers(m_device, buffers);
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: bindBuffers failed\n");
        }
    }
    if (ok) {
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(WavefrontStage::Fixed{(edgeCount + 63u) / 64u});
        runImmediate([&](VkCommandBuffer cmd) {
            stage.record(cmd);
            VkBufferMemoryBarrier barriers[2]{};
            for (int i = 0; i < 2; ++i) {
                barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barriers[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barriers[i].offset = 0;
                barriers[i].size = VK_WHOLE_SIZE;
            }
            barriers[0].buffer = flagBuffer.buffer;
            barriers[1].buffer = countBuffer.buffer;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, barriers, 0,
                                 nullptr);
        });
        const auto* flagsMapped = static_cast<const std::uint32_t*>(flagBuffer.getMappedData());
        const auto* countMapped = static_cast<const std::uint32_t*>(countBuffer.getMappedData());
        if (flagsMapped == nullptr || countMapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runSilhouetteProbe: a result buffer is not "
                                  "host mapped\n");
            ok = false;
        } else {
            outFlags.assign(flagsMapped, flagsMapped + edgeCount);
            outCount = countMapped[0];
        }
    }

    stage.destroy(m_device);
    if (countBuffer.isValid()) m_allocator.destroyBuffer(countBuffer);
    if (flagBuffer.isValid()) m_allocator.destroyBuffer(flagBuffer);
    if (triBuffer.isValid()) m_allocator.destroyBuffer(triBuffer);
    if (edgeBuffer.isValid()) m_allocator.destroyBuffer(edgeBuffer);
    if (posBuffer.isValid()) m_allocator.destroyBuffer(posBuffer);
    return ok;
}

}  // namespace ohao::diff
