// The Adam probe (Stage 2 Task 3): one optimizer_adam.comp dispatch per
// step, over caller-owned parameter, gradient and state arrays.
//
// The step index is the CALLER's, and (1 - beta^t) is computed here rather
// than in the shader -- see optimizer_adam.comp's header for why that is one
// value pushed and not two computed.
#include "gpu_probe_context.hpp"

#include "diff/wavefront/wavefront_stage.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ohao::diff {

bool GpuProbeContext::runAdamProbe(std::vector<float>& params, const std::vector<float>& grads,
                                   std::vector<float>& state, const AdamOptions& options,
                                   std::uint32_t stepIndex) {
    if (params.empty() || grads.size() != params.size() || state.size() != params.size() * 2u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runAdamProbe: params %zu, grads %zu, state %zu. The "
                     "gradient must match the parameter element for element and the state must "
                     "be exactly twice as long (m then v, which is ParamRegistry's own "
                     "stateBlock layout)\n",
                     params.size(), grads.size(), state.size());
        return false;
    }
    if (stepIndex == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runAdamProbe: stepIndex is 1-based (Kingma & Ba's t), "
                     "and t = 0 would divide by 1 - beta^0 = 0\n");
        return false;
    }
    const auto floatCount = static_cast<std::uint32_t>(params.size());

    GpuBuffer paramBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(params), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer gradBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(grads), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer stateBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(state), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    bool ok = paramBuffer.isValid() && gradBuffer.isValid() && stateBuffer.isValid();
    if (!ok) std::fprintf(stderr, "[GpuProbeContext] runAdamProbe: buffer allocation failed\n");

    struct AdamPush {
        std::uint32_t floatCount;
        float alpha;
        float beta1;
        float beta2;
        float epsilon;
        float oneMinusBeta1PowT;
        float oneMinusBeta2PowT;
    } push{floatCount,
           options.alpha,
           options.beta1,
           options.beta2,
           options.epsilon,
           // In double, then narrowed once: beta^t underflows slowly and the
           // host is the only place this is computed.
           static_cast<float>(1.0 - std::pow(static_cast<double>(options.beta1),
                                             static_cast<double>(stepIndex))),
           static_cast<float>(1.0 - std::pow(static_cast<double>(options.beta2),
                                             static_cast<double>(stepIndex)))};

    WavefrontStage stage;
    if (ok) {
        const VkDescriptorType bindings[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        ok = stage.build(m_device, "diff_optimizer_adam.comp.spv", bindings, sizeof(AdamPush));
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runAdamProbe: build failed\n");
    }
    if (ok) {
        const VkBuffer buffers[3] = {paramBuffer.buffer, gradBuffer.buffer, stateBuffer.buffer};
        ok = stage.bindBuffers(m_device, buffers);
        if (!ok) std::fprintf(stderr, "[GpuProbeContext] runAdamProbe: bindBuffers failed\n");
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
            barriers[0].buffer = paramBuffer.buffer;
            barriers[1].buffer = stateBuffer.buffer;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, barriers, 0,
                                 nullptr);
        });
        const auto* paramMapped = static_cast<const float*>(paramBuffer.getMappedData());
        const auto* stateMapped = static_cast<const float*>(stateBuffer.getMappedData());
        if (paramMapped == nullptr || stateMapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runAdamProbe: a result buffer is not host "
                                  "mapped\n");
            ok = false;
        } else {
            params.assign(paramMapped, paramMapped + params.size());
            state.assign(stateMapped, stateMapped + state.size());
        }
    }

    stage.destroy(m_device);
    if (stateBuffer.isValid()) m_allocator.destroyBuffer(stateBuffer);
    if (gradBuffer.isValid()) m_allocator.destroyBuffer(gradBuffer);
    if (paramBuffer.isValid()) m_allocator.destroyBuffer(paramBuffer);
    return ok;
}

}  // namespace ohao::diff
