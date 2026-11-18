#include "render/graph/pass.hpp"

#include "render/frame/gpu_pass_timer.hpp"

#include <algorithm>

namespace ohao::render {

namespace {

constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkPipelineStageFlags2 kDepthTests =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

} // namespace

Timed::Timed(const Frame& frame, const char* name) : m_frame(frame) {
    if (m_frame.timer) m_frame.timer->begin(m_frame.cmd, name);
}

Timed::~Timed() {
    if (m_frame.timer) m_frame.timer->end(m_frame.cmd);
}

Pass::Pass(const Frame& frame, const char* name, Kind kind) : m_frame(frame), m_name(name), m_kind(kind) {}

VkPipelineStageFlags2 Pass::shaderStages() const noexcept {
    switch (m_kind) {
    case Compute: return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case Raster:
        return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
               (m_meshShaders ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT
                              : VkPipelineStageFlags2{0});
    case RayTracing: return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    case Transfer: return kTransfer;
    }
    return 0;
}

void Pass::image(gpu::Image& img, Sub sub, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
                 bool write, bool discard) {
    if (!img) return;
    const uint32_t mip0 = sub.mip == gpu::Image::kAll ? 0u : sub.mip;
    const uint32_t mipN = sub.mip == gpu::Image::kAll ? img.desc.mips : 1u;
    const uint32_t layer0 = sub.layer == gpu::Image::kAll ? 0u : sub.layer;
    const uint32_t layerN = sub.layer == gpu::Image::kAll ? img.desc.layers : 1u;
    for (uint32_t layer = layer0; layer < layer0 + layerN; ++layer)
        for (uint32_t mip = mip0; mip < mip0 + mipN; ++mip)
            m_barriers.image(img.image, img.aspect(), img.state(mip, layer), mip, layer, layout, stage, access, write,
                             discard);
}

void Pass::buffer(GpuBuffer& buf, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write) {
    if (buf.buffer != VK_NULL_HANDLE) m_barriers.buffer(buf.state, stage, access, write);
}

Pass& Pass::sampled(gpu::Image& img, Sub sub) {
    const VkImageLayout layout = gpu::isDepthFormat(img.desc.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image(img, sub, layout, shaderStages(), VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false);
    return *this;
}

Pass& Pass::sampledGeneral(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_GENERAL, shaderStages(), VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false);
    return *this;
}

Pass& Pass::storageRead(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_GENERAL, shaderStages(), VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false, false);
    return *this;
}

Pass& Pass::storageWrite(gpu::Image& img, Old old, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_GENERAL, shaderStages(), VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true,
          old == Old::Discard);
    return *this;
}

Pass& Pass::storageReadWrite(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_GENERAL, shaderStages(),
          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, false);
    return *this;
}

Pass& Pass::external(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_GENERAL, shaderStages(),
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          true, false);
    return *this;
}

Pass& Pass::copySrc(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT, false, false);
    return *this;
}

Pass& Pass::copyDst(gpu::Image& img, Old old, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true,
          old == Old::Discard);
    return *this;
}

Pass& Pass::present(gpu::Image& img) {
    image(img, {}, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0, true, false);
    return *this;
}

Pass& Pass::color(gpu::Image& img, Load load, VkClearColorValue clear, uint32_t mip, uint32_t layer) {
    if (!img) return *this;
    const VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                  (load == Load::Keep ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : VkAccessFlags2{0});
    image(img, {mip, layer}, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          access, true, load != Load::Keep);
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = img.subView(mip, layer);
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = load == Load::Keep ? VK_ATTACHMENT_LOAD_OP_LOAD
               : load == Load::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue.color = clear;
    m_colors.push_back(a);
    m_area = img.extent(mip);
    m_layerCount = layer == gpu::Image::kAll ? img.desc.layers : 1u;
    return *this;
}

Pass& Pass::depth(gpu::Image& img, Load load, float clear, uint32_t layer) {
    if (!img) return *this;
    image(img, {0, layer}, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthTests,
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, true,
          load != Load::Keep);
    m_depth = {};
    m_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    m_depth.imageView = img.subView(0, layer);
    m_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    m_depth.loadOp = load == Load::Keep ? VK_ATTACHMENT_LOAD_OP_LOAD
                     : load == Load::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    m_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_depth.clearValue.depthStencil = {clear, 0};
    m_hasDepth = true;
    m_area = img.extent(0);
    m_layerCount = layer == gpu::Image::kAll ? img.desc.layers : 1u;
    return *this;
}

Pass& Pass::depthRead(gpu::Image& img, uint32_t layer) {
    if (!img) return *this;
    image(img, {0, layer}, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, kDepthTests | shaderStages(),
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false);
    m_depth = {};
    m_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    m_depth.imageView = img.subView(0, layer);
    m_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    m_depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    m_depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    m_hasDepth = true;
    m_area = img.extent(0);
    return *this;
}

Pass& Pass::read(GpuBuffer& buf, BufferUse use) {
    const VkPipelineStageFlags2 shader = shaderStages();
    switch (use) {
    case BufferUse::Uniform: buffer(buf, shader, VK_ACCESS_2_UNIFORM_READ_BIT, false); break;
    case BufferUse::Storage: buffer(buf, shader, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, false); break;
    case BufferUse::Indirect:   // a draw's or a dispatch's arguments: both are read at DRAW_INDIRECT
        buffer(buf, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false);
        break;
    case BufferUse::Vertex:
        buffer(buf, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, false);
        break;
    case BufferUse::Index: buffer(buf, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, false); break;
    case BufferUse::CopySrc:
    case BufferUse::CopyDst: buffer(buf, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT, false); break;
    case BufferUse::AccelInput:
        buffer(buf, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT, false);
        break;
    case BufferUse::Accel:
        buffer(buf, m_kind == Transfer ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : shader,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, false);
        break;
    case BufferUse::HostRead: buffer(buf, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, false); break;
    }
    return *this;
}

Pass& Pass::write(GpuBuffer& buf, BufferUse use) {
    const VkPipelineStageFlags2 shader = shaderStages();
    switch (use) {
    case BufferUse::CopySrc:
    case BufferUse::CopyDst: buffer(buf, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true); break;
    case BufferUse::Accel:
        buffer(buf, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
               VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, true);
        break;
    default:
        buffer(buf, shader, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, true);
        break;
    }
    return *this;
}

Pass& Pass::readWrite(GpuBuffer& buf, BufferUse use) {
    if (use != BufferUse::CopySrc && use != BufferUse::CopyDst) return write(buf, use);
    buffer(buf, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    return *this;
}

Pass& Pass::leaves(gpu::Image& img, VkImageLayout layout) {
    if (img) m_leaves.push_back({&img, layout});
    return *this;
}

void Pass::begin() {
    if (m_begun) return;
    m_begun = true;
    m_barriers.record(m_frame.cmd);
    if (m_name && m_frame.timer) m_frame.timer->begin(m_frame.cmd, m_name);
    if (m_colors.empty() && !m_hasDepth) return;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, m_area};
    info.layerCount = m_layerCount;
    info.colorAttachmentCount = static_cast<uint32_t>(m_colors.size());
    info.pColorAttachments = m_colors.data();
    info.pDepthAttachment = m_hasDepth ? &m_depth : nullptr;
    vkCmdBeginRendering(m_frame.cmd, &info);
    m_rendering = true;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_area.width), static_cast<float>(m_area.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, m_area};
    vkCmdSetViewport(m_frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(m_frame.cmd, 0, 1, &scissor);
}

void Pass::end() {
    if (!m_begun || m_ended) return;
    m_ended = true;
    if (m_rendering) vkCmdEndRendering(m_frame.cmd);
    if (m_name && m_frame.timer) m_frame.timer->end(m_frame.cmd);
    const VkPipelineStageFlags2 stage = shaderStages();
    for (const Left& left : m_leaves)
        left.image->assume(left.layout, stage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace ohao::render
