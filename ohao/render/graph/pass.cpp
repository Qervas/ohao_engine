#include "render/graph/pass.hpp"

#include "render/frame/gpu_pass_timer.hpp"

#include <algorithm>

namespace ohao::render {

namespace {

constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkPipelineStageFlags2 kDepthTests =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// The barrier between what last touched a resource and its next use, or nothing when none is needed; `s` becomes
// the state after the use. A read joins the reads since the last write; a write waits for all of them.
struct Need {
    bool barrier{false};
    VkPipelineStageFlags2 srcStage{0};
    VkAccessFlags2 srcAccess{0};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED};
};

Need advance(gpu::State& s, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write,
             bool discard) {
    Need n{};
    n.srcStage = s.writeStage | s.readStages;
    n.srcAccess = s.writeAccess;
    n.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : s.layout;
    if (write || s.layout != layout) {
        n.barrier = n.srcStage != 0 || s.layout != layout || discard;
        if (write) {
            s = gpu::State{layout, stage, access, 0, 0};
        } else {   // a change of layout for a read: the readers so far were waited for; the write stays the write
            s.layout = layout;
            s.readStages = stage;
            s.readAccess = access;
        }
        return n;
    }
    if ((stage & ~s.readStages) != 0 || (access & ~s.readAccess) != 0) {   // a reader the write is not visible to yet
        n.barrier = s.writeStage != 0;
        n.srcStage = s.writeStage;
        s.readStages |= stage;
        s.readAccess |= access;
    }
    return n;
}

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
    case Raster: return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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
    for (uint32_t layer = layer0; layer < layer0 + layerN; ++layer) {
        for (uint32_t mip = mip0; mip < mip0 + mipN; ++mip) {
            const Need n = advance(img.state(mip, layer), layout, stage, access, write, discard);
            if (!n.barrier) continue;
            // One barrier for a run of mips of a layer that need the same thing (the common case: all of them).
            if (!m_imageBarriers.empty()) {
                VkImageMemoryBarrier2& last = m_imageBarriers.back();
                const auto& r = last.subresourceRange;
                if (last.image == img.image && last.srcStageMask == n.srcStage && last.srcAccessMask == n.srcAccess &&
                    last.oldLayout == n.oldLayout && last.newLayout == layout && last.dstStageMask == stage &&
                    last.dstAccessMask == access && r.baseArrayLayer == layer && r.layerCount == 1 &&
                    r.baseMipLevel + r.levelCount == mip) {
                    ++last.subresourceRange.levelCount;
                    continue;
                }
                // ... or the next layer of the same mips.
                if (last.image == img.image && last.srcStageMask == n.srcStage && last.srcAccessMask == n.srcAccess &&
                    last.oldLayout == n.oldLayout && last.newLayout == layout && last.dstStageMask == stage &&
                    last.dstAccessMask == access && mipN == 1 && r.baseMipLevel == mip && r.levelCount == 1 &&
                    r.baseArrayLayer + r.layerCount == layer) {
                    ++last.subresourceRange.layerCount;
                    continue;
                }
            }
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = n.srcStage;
            b.srcAccessMask = n.srcAccess;
            b.dstStageMask = stage;
            b.dstAccessMask = access;
            b.oldLayout = n.oldLayout;
            b.newLayout = layout;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {img.aspect(), mip, 1, layer, 1};
            m_imageBarriers.push_back(b);
        }
    }
}

void Pass::buffer(GpuBuffer& buf, VkPipelineStageFlags2 stage, VkAccessFlags2 access, bool write) {
    if (buf.buffer == VK_NULL_HANDLE) return;
    const Need n = advance(buf.state, VK_IMAGE_LAYOUT_UNDEFINED, stage, access, write, false);
    if (!n.barrier || n.srcStage == 0) return;
    VkBufferMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask = n.srcStage;
    b.srcAccessMask = n.srcAccess;
    b.dstStageMask = stage;
    b.dstAccessMask = access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf.buffer;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    m_bufferBarriers.push_back(b);
}

Pass& Pass::sampled(gpu::Image& img, Sub sub) {
    image(img, sub, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, shaderStages(), VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false,
          false);
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
    case BufferUse::Indirect:
        buffer(buf, m_kind == Compute ? shader : VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, false);
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

Pass& Pass::leaves(gpu::Image& img, VkImageLayout layout) {
    if (img) m_leaves.push_back({&img, layout});
    return *this;
}

void Pass::begin() {
    if (m_begun) return;
    m_begun = true;
    if (!m_imageBarriers.empty() || !m_bufferBarriers.empty()) {
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(m_bufferBarriers.size());
        dep.pBufferMemoryBarriers = m_bufferBarriers.data();
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(m_imageBarriers.size());
        dep.pImageMemoryBarriers = m_imageBarriers.data();
        vkCmdPipelineBarrier2(m_frame.cmd, &dep);
    }
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
