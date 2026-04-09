#pragma once
#include "types.hpp"
#include <glm/glm.hpp>

namespace ohao::gpu {

// Abstract command list — records GPU commands for deferred execution
// Implementations: VulkanCommandList, CudaCommandList (future)
class CommandList {
public:
    virtual ~CommandList() = default;

    // Lifecycle
    virtual void begin() = 0;
    virtual void end() = 0;

    // Pipeline binding
    virtual void bindPipeline(PipelineHandle pipeline) = 0;

    // Resource binding
    virtual void bindBuffer(uint32_t binding, BufferHandle buffer) = 0;
    virtual void bindImage(uint32_t binding, ImageHandle image) = 0;
    virtual void bindSampler(uint32_t binding, SamplerHandle sampler, ImageHandle image) = 0;
    virtual void bindAccelerationStructure(uint32_t binding, AccelHandle accel) = 0;

    // Push constants
    virtual void pushConstants(const void* data, uint32_t size) = 0;

    // Dispatch
    virtual void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;
    virtual void traceRays(uint32_t width, uint32_t height, uint32_t depth) = 0;

    // Transfer
    virtual void copyBufferToBuffer(BufferHandle src, BufferHandle dst, uint64_t size) = 0;
    virtual void copyImageToBuffer(ImageHandle src, BufferHandle dst) = 0;
    virtual void copyBufferToImage(BufferHandle src, ImageHandle dst, uint32_t layer = 0) = 0;

    // Barriers
    virtual void imageBarrier(ImageHandle image, ImageUsage from, ImageUsage to) = 0;
    virtual void bufferBarrier(BufferHandle buffer) = 0;

    // Submit to GPU
    virtual void submit() = 0;
    virtual void submitAndWait() = 0;
};

} // namespace ohao::gpu
