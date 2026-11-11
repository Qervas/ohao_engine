#pragma once
// One pass of a frame (system design 4.6, step 4a). A pass says what it reads and writes; begin() derives every
// barrier from what last touched each resource (gpu::State, kept on the resource across passes and frames),
// records them in one call, opens the pass's timer and, when attachments were declared, begins the rendering.
// Nothing else in the renderer records a barrier, a render pass or a framebuffer: tools/check.py refuses one.
//
//     render::Pass pass(frame, "lighting", render::Pass::Raster);
//     pass.sampled(normal).sampled(albedo).read(lights, render::BufferUse::Storage)
//         .color(lit, render::Load::Clear);
//     pass.begin();
//     ... bind, draw ...
//     pass.end();        // or the destructor
//
// Passes run in the order the frame's code writes them: there is no reordering and no culling (a feature that is
// off does not make its pass). An image whose old contents are not wanted says Discard.
#include "gpu/vulkan/gpu_allocator.hpp"
#include "gpu/vulkan/gpu_image.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace ohao::render {

class GpuPassTimer;

// What every pass of a frame records into.
struct Frame {
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    GpuPassTimer* timer{nullptr};
    uint32_t slot{0};   // the frame-in-flight slot
};

enum class Load : uint8_t { Keep, Clear, Discard };
enum class Old : uint8_t { Keep, Discard };

enum class BufferUse : uint8_t {
    Uniform, Storage, Indirect, Vertex, Index, CopySrc, CopyDst,
    AccelInput,   // geometry or instances an acceleration-structure build reads
    Accel,        // an acceleration structure's storage: written by a build, read by a trace or a ray query
    HostRead,     // mapped memory the host reads once the frame's fence has signalled
};

// One timer over several passes (bloom's fifteen are post.bloom): the passes inside take a null name.
class Timed {
public:
    Timed(const Frame& frame, const char* name);
    ~Timed();
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

private:
    Frame m_frame;
};

// A range of an image's subresources; the default is all of it.
struct Sub {
    uint32_t mip{gpu::Image::kAll};
    uint32_t layer{gpu::Image::kAll};
};

class Pass {
public:
    enum Kind : uint8_t { Compute, Raster, RayTracing, Transfer };

    // `name`: a render/frame/pass_registry.hpp name the pass is timed under, or null (not timed, no breadcrumb).
    Pass(const Frame& frame, const char* name, Kind kind);
    ~Pass() { end(); }
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;

    // A raster pass that also draws with task and mesh shaders (VK_EXT_mesh_shader enabled): they read what its
    // other shaders read. Before the declarations.
    Pass& meshShaders(bool on = true) {
        m_meshShaders = on;
        return *this;
    }

    // --- images
    // A texture a shader samples. A depth image is sampled in DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout
    // depthRead() tests it in; any other in SHADER_READ_ONLY_OPTIMAL.
    Pass& sampled(gpu::Image& image, Sub sub = {});
    Pass& storageRead(gpu::Image& image, Sub sub = {});
    Pass& storageWrite(gpu::Image& image, Old old = Old::Discard, Sub sub = {});
    Pass& storageReadWrite(gpu::Image& image, Sub sub = {});
    Pass& copySrc(gpu::Image& image, Sub sub = {});
    Pass& copyDst(gpu::Image& image, Old old = Old::Discard, Sub sub = {});
    Pass& present(gpu::Image& image);                          // left ready for the swapchain
    // Attachments: one mip of one layer each (`layer` kAll on a layered image: every layer, for a multiview or
    // layered draw).
    Pass& color(gpu::Image& image, Load load, VkClearColorValue clear = {}, uint32_t mip = 0, uint32_t layer = 0);
    Pass& depth(gpu::Image& image, Load load, float clear = 0.0f, uint32_t layer = 0);
    Pass& depthRead(gpu::Image& image, uint32_t layer = 0);    // tested against, not written; may also be sampled

    // --- buffers
    Pass& read(GpuBuffer& buffer, BufferUse use);
    Pass& write(GpuBuffer& buffer, BufferUse use);
    Pass& readWrite(GpuBuffer& buffer, BufferUse use) { return write(buffer, use); }

    // --- what code with barriers of its own (NRD, DLSS) leaves behind: after end(), `image` is in `layout`.
    Pass& leaves(gpu::Image& image, VkImageLayout layout);

    void begin();
    void end();

    [[nodiscard]] VkCommandBuffer cmd() const noexcept { return m_frame.cmd; }
    [[nodiscard]] VkExtent2D area() const noexcept { return m_area; }

private:
    void image(gpu::Image& image, Sub sub, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
               bool write, bool discard);
    void buffer(GpuBuffer& buffer, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write);
    [[nodiscard]] VkPipelineStageFlags2 shaderStages() const noexcept;

    struct Left { gpu::Image* image; VkImageLayout layout; };

    Frame m_frame;
    const char* m_name;
    Kind m_kind;
    bool m_meshShaders{false};
    bool m_begun{false};
    bool m_ended{false};
    bool m_rendering{false};
    VkExtent2D m_area{};
    std::vector<VkImageMemoryBarrier2> m_imageBarriers;
    std::vector<VkBufferMemoryBarrier2> m_bufferBarriers;
    std::vector<VkRenderingAttachmentInfo> m_colors;
    VkRenderingAttachmentInfo m_depth{};
    bool m_hasDepth{false};
    uint32_t m_layerCount{1};
    std::vector<Left> m_leaves;
};

} // namespace ohao::render
