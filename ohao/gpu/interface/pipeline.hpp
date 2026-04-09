#pragma once
#include "types.hpp"
#include <vector>
#include <string>

namespace ohao::gpu {

// Shader module — loaded SPIR-V / PTX / DXIL
struct ShaderModule {
    ShaderStage stage;
    std::vector<uint8_t> code;     // compiled bytecode (SPIR-V, PTX, etc.)
    std::string entryPoint = "main";
};

// Ray tracing pipeline descriptor
struct RTPipelineDesc {
    ShaderModule rayGen;
    ShaderModule closestHit;
    ShaderModule miss;
    ShaderModule anyHit;            // optional
    uint32_t maxRecursionDepth = 8;
    uint32_t maxPayloadSize = 128;  // bytes
    const char* debugName = nullptr;
};

// Compute pipeline descriptor
struct ComputePipelineDesc {
    ShaderModule compute;
    const char* debugName = nullptr;
};

// Acceleration structure geometry input
struct AccelGeometryDesc {
    BufferHandle vertexBuffer;
    BufferHandle indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t vertexStride = 0;
    uint32_t indexCount = 0;
    uint64_t vertexOffset = 0;      // byte offset in buffer
    uint64_t indexOffset = 0;       // byte offset in buffer
};

// Acceleration structure instance
struct AccelInstanceDesc {
    AccelHandle blas;
    float transform[12];            // 3x4 row-major transform
    uint32_t customIndex = 0;       // gl_InstanceCustomIndexEXT
    uint32_t mask = 0xFF;
};

// Abstract pipeline factory — creates pipelines and acceleration structures
// Lives on Device, but separated for clarity
class PipelineFactory {
public:
    virtual ~PipelineFactory() = default;

    // Pipelines
    virtual PipelineHandle createRTPipeline(const RTPipelineDesc& desc) = 0;
    virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual void destroyPipeline(PipelineHandle handle) = 0;

    // Acceleration structures
    virtual AccelHandle createBLAS(const AccelGeometryDesc& desc) = 0;
    virtual AccelHandle createTLAS(const std::vector<AccelInstanceDesc>& instances) = 0;
    virtual void destroyAccel(AccelHandle handle) = 0;
    virtual void rebuildTLAS(AccelHandle tlas, const std::vector<AccelInstanceDesc>& instances) = 0;
};

} // namespace ohao::gpu
