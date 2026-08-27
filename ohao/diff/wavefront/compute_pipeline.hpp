#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ohao::diff {

/// RAII wrapper around the compute-pipeline creation sequence every
/// wavefront stage repeats: SPIR-V module -> descriptor set layout ->
/// pipeline layout -> pipeline -> descriptor pool -> descriptor set.
///
/// Lifted from tests/diff/gpu_probe_context.cpp's
/// dispatchStorageBufferCompute, which is the reference for both the
/// creation sequence and its failure-path discipline: every early return
/// releases exactly what has been created so far, in reverse order.
/// dispatchStorageBufferCompute itself is now implemented on top of this
/// class (see gpu_probe_context.cpp).
///
/// Ownership mirrors GradientArena and WavefrontBuffers in this subsystem:
/// build()/destroy() are the explicit lifecycle, the destructor is a
/// backstop for early-return error paths (it stashes the VkDevice passed to
/// build() so it has something to call destroy() with), and copy/move are
/// deleted because the compiler-generated versions would copy the raw
/// Vulkan handles, leaving two objects that each believe they own (and will
/// each destroy) the same pipeline/layout/set.
class ComputePipeline {
public:
    ComputePipeline() = default;
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&) = delete;
    ComputePipeline& operator=(ComputePipeline&&) = delete;

    /// Loads `spvName`'s compiled SPIR-V (searched via the same
    /// candidate-path scheme tests/diff/gpu_probe_context.cpp's loadSpv
    /// used -- moved into this class rather than left in the harness
    /// because build() takes a name, not bytecode: every current and
    /// future caller loads from the same compiled-shaders output
    /// directory, so there is no case that needs bytecode handed in
    /// directly, and duplicating a ~25-line path-search helper is cheaper
    /// than plumbing bytecode through every call site).
    ///
    /// Builds a descriptor set layout with one binding per entry of
    /// `bindings` (binding index == span index -- see bindBuffers'
    /// comment for the convention this implies), a pipeline layout with a
    /// single push-constant range of `pushConstantSize` bytes (the range
    /// is omitted entirely when `pushConstantSize` is 0, since a
    /// zero-size VkPushConstantRange is invalid usage), the compute
    /// pipeline itself, a descriptor pool sized for exactly the requested
    /// bindings, and one descriptor set allocated from it.
    ///
    /// On any failure, releases everything created so far (reverse
    /// order) before returning false -- see dispatchStorageBufferCompute
    /// for the precedent this preserves.
    [[nodiscard]] bool build(VkDevice device, const char* spvName,
                             std::span<const VkDescriptorType> bindings,
                             uint32_t pushConstantSize);

    /// Idempotent: safe to call when already destroyed, or never built.
    void destroy(VkDevice device);

    [[nodiscard]] VkPipeline pipeline() const noexcept { return m_pipeline; }
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return m_pipelineLayout; }
    [[nodiscard]] VkDescriptorSet descriptorSet() const noexcept { return m_descriptorSet; }

    /// Writes `buffers[i]` as a VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    /// descriptor at binding `i` (0-based) -- i.e. `buffers` is assumed to
    /// list bindings starting at index 0 in the same order build()'s
    /// `bindings` span declared them. A layout that mixes storage buffers
    /// with an acceleration structure (see bindAccelerationStructure)
    /// should list its storage-buffer bindings starting at 0 and pass only
    /// that prefix here.
    ///
    /// Returns false if `buffers.size()` exceeds the binding count
    /// build() was given, or if any of those bindings was not declared
    /// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER.
    [[nodiscard]] bool bindBuffers(VkDevice device, std::span<const VkBuffer> buffers);

    /// Writes a VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR descriptor at
    /// `binding`. Returns false if `binding` is out of range or was not
    /// declared that type in build().
    [[nodiscard]] bool bindAccelerationStructure(VkDevice device, uint32_t binding,
                                                 VkAccelerationStructureKHR accel);

private:
    // Stashed from build() purely so the destructor backstop has something
    // to call destroy() with; destroy() itself always takes its device as
    // an explicit parameter, matching GradientArena/WavefrontBuffers.
    VkDevice m_device{VK_NULL_HANDLE};

    VkShaderModule m_shaderModule{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Recorded by build() so bindBuffers/bindAccelerationStructure can
    // validate the binding index against the type it was actually declared
    // with, and so the descriptor pool can be sized without re-deriving
    // this from anywhere else. Cleared by destroy().
    std::vector<VkDescriptorType> m_bindingTypes;
};

}  // namespace ohao::diff
