#include "diff/wavefront/gradient_stages.hpp"

#include <array>
#include <cstdio>

namespace ohao::diff {

bool GradientStages::build(VkDevice device, const PushSizes& sizes) {
    if (m_built) return true;
    if (device == VK_NULL_HANDLE || !sizes.valid()) return false;

    constexpr VkDescriptorType kBuf = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr VkDescriptorType kAccel = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    // State, queue, counter.
    const std::array<VkDescriptorType, 3> kStateQueueCounter = {kBuf, kBuf, kBuf};
    const std::array<VkDescriptorType, 1> kCounterOnly = {kBuf};
    // ... plus the scene's vertex and index buffers, and the TLAS at 5.
    const std::array<VkDescriptorType, 6> kIntersect = {kBuf, kBuf, kBuf, kBuf, kBuf, kAccel};
    // BOTH INSTANTIATIONS DECLARE THE SAME THIRTEEN, because both include the
    // same traverse.glsl. 8 is the TLAS, 10 the gradient arena, 11 the
    // emission-texture primal, 12 the adjoint seed.
    const std::array<VkDescriptorType, 13> kScatter = {kBuf,   kBuf, kBuf, kBuf, kBuf,
                                                      kBuf,   kBuf, kBuf, kAccel, kBuf,
                                                      kBuf,   kBuf, kBuf};

    const bool ok =
        m_generate.build(device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                         sizes.generate) &&
        m_prepareIndirect.build(device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                sizes.prepareIndirect) &&
        m_intersect.build(device, "diff_wf_intersect.comp.spv", kIntersect, sizes.intersect) &&
        m_scatterForward.build(device, "diff_wf_scatter.comp.spv", kScatter, sizes.scatter) &&
        // The REPLAY instantiation: a different SPV, the same declared
        // bindings. If that ever stops being true, this shared table is the
        // first thing to look at.
        m_scatterReplay.build(device, "diff_wf_scatter_replay.comp.spv", kScatter,
                              sizes.scatter);
    if (!ok) {
        std::fprintf(stderr, "[GradientStages] build failed\n");
        destroy(device);
        return false;
    }
    m_built = true;
    return true;
}

void GradientStages::destroy(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;
    // Reverse order of creation, as everything else in this subsystem tears
    // down -- WavefrontStage::destroy is idempotent, so a partially built set
    // is safe to hand here.
    m_scatterReplay.destroy(device);
    m_scatterForward.destroy(device);
    m_intersect.destroy(device);
    m_prepareIndirect.destroy(device);
    m_generate.destroy(device);
    m_built = false;
}

}  // namespace ohao::diff
