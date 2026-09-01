// The loss probe (Stage 2 Task 2): dispatch loss_l2.comp over a film and a
// target, read back dL/d(film) and the scalar loss.
//
// Deliberately NOT part of any wavefront probe. The loss is a separate
// kernel with its own bindings and its own dispatch, and keeping its entry
// point separate is the structural half of spec 4.6's rule that the loss may
// not reach into the integrator: there is no code path here that can touch a
// path, a queue or an arena, because none of those is bound.
#include "gpu_probe_context.hpp"

#include "diff/wavefront/wavefront_stage.hpp"

#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GpuProbeContext::runLossL2Probe(const std::vector<float>& film,
                                     const std::vector<float>& target,
                                     std::vector<float>& outSeed, double& outLoss) {
    outSeed.clear();
    outLoss = 0.0;
    if (film.empty() || film.size() != target.size()) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runLossL2Probe: film holds %zu floats and target %zu. "
                     "They must be the same non-empty length -- the loss compares them element "
                     "for element, so a length mismatch is not a smaller comparison, it is a "
                     "different one\n",
                     film.size(), target.size());
        return false;
    }
    const auto floatCount = static_cast<std::uint32_t>(film.size());

    GpuBuffer filmBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(film), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer targetBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(target), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const std::vector<float> zeroSeed(film.size(), 0.0f);
    GpuBuffer seedBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(zeroSeed), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // ONE float, zeroed: the shader atomicAdds into slot 0, so it accumulates
    // onto whatever is already there and the caller would otherwise be
    // measuring this run plus the last one.
    const std::vector<float> zeroLoss{0.0f};
    GpuBuffer lossBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(zeroLoss), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    bool ok = filmBuffer.isValid() && targetBuffer.isValid() && seedBuffer.isValid() &&
              lossBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runLossL2Probe: buffer allocation failed\n");
    }

    WavefrontStage stage;
    struct LossPush {
        std::uint32_t floatCount;
    } push{floatCount};

    if (ok) {
        const VkDescriptorType bindings[4] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        ok = stage.build(m_device, "diff_loss_l2.comp.spv", bindings, sizeof(LossPush));
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runLossL2Probe: build failed\n");
        }
    }
    if (ok) {
        const VkBuffer buffers[4] = {filmBuffer.buffer, targetBuffer.buffer, seedBuffer.buffer,
                                     lossBuffer.buffer};
        ok = stage.bindBuffers(m_device, buffers);
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runLossL2Probe: bindBuffers failed\n");
        }
    }
    if (ok) {
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(WavefrontStage::Fixed{(floatCount + 63u) / 64u});
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
            barriers[0].buffer = seedBuffer.buffer;
            barriers[1].buffer = lossBuffer.buffer;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, barriers, 0,
                                 nullptr);
        });
        // Host-visible and mapped, like every other probe buffer here.
        const auto* seedMapped = static_cast<const float*>(seedBuffer.getMappedData());
        const auto* lossMapped = static_cast<const float*>(lossBuffer.getMappedData());
        if (seedMapped == nullptr || lossMapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runLossL2Probe: a result buffer is not host "
                                  "mapped, so nothing can be read back\n");
            ok = false;
        } else {
            outSeed.assign(seedMapped, seedMapped + film.size());
            outLoss = static_cast<double>(lossMapped[0]);
        }
    }

    stage.destroy(m_device);
    if (lossBuffer.isValid()) m_allocator.destroyBuffer(lossBuffer);
    if (seedBuffer.isValid()) m_allocator.destroyBuffer(seedBuffer);
    if (targetBuffer.isValid()) m_allocator.destroyBuffer(targetBuffer);
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    return ok;
}

}  // namespace ohao::diff
