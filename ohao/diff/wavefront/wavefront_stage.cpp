#include "diff/wavefront/wavefront_stage.hpp"

#include <cassert>
#include <cstring>
#include <type_traits>

namespace ohao::diff {

bool WavefrontStage::build(VkDevice device, const char* spvName,
                           std::span<const VkDescriptorType> bindings,
                           uint32_t pushConstantSize) {
    // Cleared unconditionally, not just on success: even a successful
    // build() allocates a fresh descriptor set that has not been written
    // yet, so any bound-ness from a previous build()/bind() cycle on this
    // object no longer applies.
    m_bound = false;
    return m_pipeline.build(device, spvName, bindings, pushConstantSize);
}

void WavefrontStage::destroy(VkDevice device) {
    m_pipeline.destroy(device);
    m_pushConstants.clear();
    m_groupCount = Fixed{0};
    m_bound = false;
}

bool WavefrontStage::bindBuffers(VkDevice device, std::span<const VkBuffer> buffers) {
    const bool ok = m_pipeline.bindBuffers(device, buffers);
    if (ok) m_bound = true;
    return ok;
}

bool WavefrontStage::bindAccelerationStructure(VkDevice device, uint32_t binding,
                                               VkAccelerationStructureKHR accel) {
    const bool ok = m_pipeline.bindAccelerationStructure(device, binding, accel);
    if (ok) m_bound = true;
    return ok;
}

void WavefrontStage::setPushConstants(const void* data, uint32_t size) {
    m_pushConstants.resize(size);
    if (size > 0) {
        std::memcpy(m_pushConstants.data(), data, size);
    }
}

void WavefrontStage::setGroupCount(GroupCountSource source) {
    m_groupCount = source;
}

void WavefrontStage::record(VkCommandBuffer cmd) const {
    assert(m_pipeline.pipeline() != VK_NULL_HANDLE && m_pipeline.layout() != VK_NULL_HANDLE &&
          m_pipeline.descriptorSet() != VK_NULL_HANDLE && m_bound &&
          "WavefrontStage::record: build() must have succeeded, destroy() must not have been "
          "called since, and a bindBuffers()/bindAccelerationStructure() call must have "
          "succeeded since that build(), before record() is called");

    // Unconditional guard backing the assert above, same pattern as
    // ArenaLayout::block() (arena_layout.cpp): the assert is a debug-time
    // diagnostic, this early return is what actually makes an unbuilt,
    // destroyed, or built-but-unbound stage's record() a safe no-op in a
    // Release build (NDEBUG compiles the assert out, but not this check).
    if (m_pipeline.pipeline() == VK_NULL_HANDLE || m_pipeline.layout() == VK_NULL_HANDLE ||
       m_pipeline.descriptorSet() == VK_NULL_HANDLE || !m_bound) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.pipeline());

    VkDescriptorSet descSet = m_pipeline.descriptorSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline.layout(), 0, 1,
                            &descSet, 0, nullptr);

    if (!m_pushConstants.empty()) {
        vkCmdPushConstants(cmd, m_pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           static_cast<uint32_t>(m_pushConstants.size()), m_pushConstants.data());
    }

    std::visit(
        [cmd](auto&& source) {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::is_same_v<T, Fixed>) {
                vkCmdDispatch(cmd, source.groups, 1, 1);
            } else {
                static_assert(std::is_same_v<T, Indirect>);
                vkCmdDispatchIndirect(cmd, source.buffer, source.offset);
            }
        },
        m_groupCount);
}

}  // namespace ohao::diff
