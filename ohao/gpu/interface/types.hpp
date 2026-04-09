#pragma once
#include <cstdint>
#include <string>

namespace ohao::gpu {

// Buffer usage flags (combinable)
enum class BufferUsage : uint32_t {
    Vertex          = 1 << 0,
    Index           = 1 << 1,
    Uniform         = 1 << 2,
    Storage         = 1 << 3,
    TransferSrc     = 1 << 4,
    TransferDst     = 1 << 5,
    AccelInput      = 1 << 6,   // acceleration structure build input
    ShaderBinding   = 1 << 7,   // shader binding table
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(BufferUsage a, BufferUsage b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Memory location
enum class MemoryLocation {
    Device,         // GPU-local (fast, not CPU-accessible)
    Host,           // CPU-accessible (staging, readback)
    Shared,         // CPU+GPU accessible (unified memory)
};

// Image format
enum class Format {
    R8_UNORM,
    RG8_UNORM,
    RGBA8_UNORM,
    RGBA8_SRGB,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    R32_FLOAT,
    D32_FLOAT,          // depth
    D24_UNORM_S8_UINT,  // depth-stencil
};

// Image type
enum class ImageType {
    Tex2D,
    Tex2DArray,
    Tex3D,
    TexCube,
};

// Image usage
enum class ImageUsage : uint32_t {
    Sampled         = 1 << 0,   // read in shader
    Storage         = 1 << 1,   // read/write in shader
    ColorAttachment = 1 << 2,   // render target
    DepthAttachment = 1 << 3,
    TransferSrc     = 1 << 4,
    TransferDst     = 1 << 5,
};
inline ImageUsage operator|(ImageUsage a, ImageUsage b) {
    return static_cast<ImageUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// Pipeline type
enum class PipelineType {
    Graphics,
    Compute,
    RayTracing,
};

// Shader stage
enum class ShaderStage {
    Vertex,
    Fragment,
    Compute,
    RayGen,
    ClosestHit,
    Miss,
    AnyHit,
};

// Handle types — lightweight IDs, backend maps to real resources
using BufferHandle = uint64_t;
using ImageHandle = uint64_t;
using PipelineHandle = uint64_t;
using AccelHandle = uint64_t;
using SamplerHandle = uint64_t;

constexpr BufferHandle InvalidBuffer = 0;
constexpr ImageHandle InvalidImage = 0;
constexpr PipelineHandle InvalidPipeline = 0;
constexpr AccelHandle InvalidAccel = 0;

// Buffer creation info
struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::Storage;
    MemoryLocation memory = MemoryLocation::Device;
    const char* debugName = nullptr;
};

// Image creation info
struct ImageDesc {
    uint32_t width = 1, height = 1, depth = 1;
    uint32_t arrayLayers = 1;
    uint32_t mipLevels = 1;
    Format format = Format::RGBA8_SRGB;
    ImageType type = ImageType::Tex2D;
    ImageUsage usage = ImageUsage::Sampled;
    const char* debugName = nullptr;
};

} // namespace ohao::gpu
