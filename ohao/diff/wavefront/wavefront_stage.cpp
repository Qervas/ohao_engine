#include "diff/wavefront/wavefront_stage.hpp"

#include <cassert>
#include <cstring>
#include <type_traits>

namespace ohao::diff {

bool WavefrontStage::build(VkDevice device, const char* spvName,
                           std::span<const VkDescriptorType> bindings,
                           uint32_t pushConstantSize) {
    return m_pipeline.build(device, spvName, bindings, pushConstantSize);
}

void WavefrontStage::destroy(VkDevice device) {
    m_pipeline.destroy(device);
    m_pushConstants.clear();
    m_groupCount = Fixed{0};
}

bool WavefrontStage::bindBuffers(VkDevice device, std::span<const VkBuffer> buffers) {
    return m_pipeline.bindBuffers(device, buffers);
}

bool WavefrontStage::bindAccelerationStructure(VkDevice device, uint32_t binding,
                                               VkAccelerationStructureKHR accel) {
    return m_pipeline.bindAccelerationStructure(device, binding, accel);
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
          m_pipeline.descriptorSet() != VK_NULL_HANDLE &&
          "WavefrontStage::record: build() must have succeeded, and destroy() must not have "
          "been called since, before record() is called");

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
