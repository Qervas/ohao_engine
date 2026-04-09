#pragma once
#include "types.hpp"
#include "command_list.hpp"
#include "pipeline.hpp"
#include <vector>
#include <memory>
#include <string>

namespace ohao::gpu {

// Abstract GPU device — one per physical GPU
// Implementations: VulkanDevice, CudaDevice (future)
class Device {
public:
    virtual ~Device() = default;

    // Device info
    virtual std::string getName() const = 0;
    virtual bool supportsRayTracing() const = 0;

    // Buffer operations
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual void destroyBuffer(BufferHandle handle) = 0;
    virtual void* mapBuffer(BufferHandle handle) = 0;
    virtual void unmapBuffer(BufferHandle handle) = 0;
    virtual void uploadBuffer(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) = 0;
    virtual uint64_t getBufferDeviceAddress(BufferHandle handle) = 0;

    // Image operations
    virtual ImageHandle createImage(const ImageDesc& desc) = 0;
    virtual void destroyImage(ImageHandle handle) = 0;
    virtual void uploadImage(ImageHandle handle, const void* data, uint32_t layer = 0) = 0;

    // Sampler
    virtual SamplerHandle createSampler(bool linear = true, bool repeat = true) = 0;
    virtual void destroySampler(SamplerHandle handle) = 0;

    // Command list
    virtual std::unique_ptr<CommandList> createCommandList() = 0;

    // Pipeline factory
    virtual PipelineFactory& getPipelineFactory() = 0;

    // Sync
    virtual void waitIdle() = 0;
};

} // namespace ohao::gpu
