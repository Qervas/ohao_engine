#include "diff/wavefront/gradient_stages.hpp"

#include "diff/wavefront/wavefront_buffers.hpp"

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
    // BOTH INSTANTIATIONS DECLARE THE SAME FIFTEEN, because both include the
    // same traverse.glsl. 8 is the TLAS, 10 the gradient arena, 11 the
    // emission-texture primal, 12 the adjoint seed, 13 the sensitivity map,
    // 14 the environment radiance image. The forward one writes neither 10
    // nor 13 -- it is pushed 0 for both lengths -- and still needs
    // descriptors for them, because a statically-used binding needs one
    // whether or not its branch runs.
    const std::array<VkDescriptorType, 15> kScatter = {kBuf, kBuf, kBuf, kBuf,   kBuf,
                                                       kBuf, kBuf, kBuf, kAccel, kBuf,
                                                       kBuf, kBuf, kBuf, kBuf,   kBuf};

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

bool GradientStages::bindAll(VkDevice device, WavefrontBuffers& buffers, const Scene& scene,
                             ScatterSinks& sinks, const Attachments& attachments) {
    if (!m_built || device == VK_NULL_HANDLE) return false;
    if (!scene.valid() || !attachments.valid() || !sinks.valid()) return false;

    const std::array<VkBuffer, 3> stateQueueCounter = {
        buffers.stateBuffer(), buffers.queueBuffer(), buffers.counterBuffer()};
    const std::array<VkBuffer, 1> counterOnly = {buffers.counterBuffer()};
    const std::array<VkBuffer, 5> intersectBuffers = {
        buffers.stateBuffer(), buffers.queueBuffer(), buffers.counterBuffer(),
        scene.vertexBuffer, scene.indexBuffer};

    bool ok = m_generate.bindBuffers(device, stateQueueCounter) &&
              m_prepareIndirect.bindBuffers(device, counterOnly) &&
              m_intersect.bindBuffers(device, intersectBuffers) &&
              m_intersect.bindAccelerationStructure(device, 5, scene.tlas);

    for (std::uint32_t variant = 0; ok && variant < 2u; ++variant) {
        ScatterSinkSet& s = (variant == 0u) ? sinks.forward() : sinks.replay();
        WavefrontStage& stage = scatter(variant);
        const std::array<VkBuffer, 8> scatterBuffers = {
            buffers.stateBuffer(),       buffers.queueBuffer(),
            buffers.counterBuffer(),     s.trace.buffer,
            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
            s.env.buffer,                s.nee.buffer};
        ok = stage.bindBuffers(device, scatterBuffers) &&
             stage.bindAccelerationStructure(device, 8, scene.tlas) &&
             stage.bindStorageBuffer(device, 9, s.film.buffer) &&
             stage.bindStorageBuffer(device, 10, attachments.gradientArena) &&
             stage.bindStorageBuffer(device, 11, attachments.emissionTexture) &&
             stage.bindStorageBuffer(device, 12, attachments.adjointSeed) &&
             stage.bindStorageBuffer(device, 13, attachments.sensitivity) &&
             stage.bindStorageBuffer(device, 14, attachments.envImage);
    }
    if (!ok) std::fprintf(stderr, "[GradientStages] descriptor binding failed\n");
    return ok;
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
