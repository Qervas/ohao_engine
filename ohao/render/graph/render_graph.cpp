#include "render/graph/render_graph.hpp"

#include "render/frame/gpu_pass_timer.hpp"

#include <unordered_map>

namespace ohao {

void PassBuilder::readTexture(TextureHandle handle, VkPipelineStageFlags stage) {
    m_graph.m_passes[m_passIndex].reads.push_back(
        {handle, false, stage, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

void PassBuilder::declareColorWrite(TextureHandle handle, VkImageLayout finalLayout) {
    m_graph.m_passes[m_passIndex].writes.push_back(
        {handle, true, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, finalLayout});
}

void PassBuilder::declareDepthWrite(TextureHandle handle, VkImageLayout finalLayout) {
    m_graph.m_passes[m_passIndex].writes.push_back(
        {handle, true, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, finalLayout});
}

void RenderGraph::shutdown() {
    m_textures.clear();
    m_passes.clear();
    m_device = VK_NULL_HANDLE;
}

void RenderGraph::addPass(std::string_view name, std::function<void(PassBuilder&)> setup,
                          std::function<void(VkCommandBuffer)> execute, bool selfTimed) {
    const auto index = static_cast<uint32_t>(m_passes.size());
    Pass pass;
    pass.name = std::string(name);
    pass.selfTimed = selfTimed;
    pass.execute = std::move(execute);
    m_passes.push_back(std::move(pass));
    PassBuilder builder(*this, index);
    setup(builder);
}

TextureHandle RenderGraph::importTexture(std::string_view, VkImage image, VkImageView, VkFormat format, uint32_t,
                                         uint32_t, VkImageLayout currentLayout) {
    m_textures.push_back({image, format, currentLayout});
    return TextureHandle{static_cast<uint32_t>(m_textures.size() - 1)};
}

void RenderGraph::compile() {
    std::unordered_map<uint32_t, Access> last;   // each texture's last access this frame
    for (Pass& pass : m_passes) {
        pass.barriers.clear();
        // A read after a write waits for it; a read of a texture not written this frame moves it from its layout.
        for (const Access& read : pass.reads) {
            if (!read.texture.isValid()) continue;
            const auto it = last.find(read.texture.index);
            const VkImageLayout current = m_textures[read.texture.index].layout;
            if (it != last.end() && it->second.write)
                pass.barriers.push_back({read.texture, it->second.stage, read.stage, it->second.access, read.access,
                                         it->second.layout, read.layout});
            else if (current != read.layout)
                pass.barriers.push_back({read.texture, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, read.stage, 0, read.access,
                                         current, read.layout});
            last[read.texture.index] = read;
        }
        // A write after any access changes the layout if it differs; a first write starts from UNDEFINED.
        for (const Access& write : pass.writes) {
            if (!write.texture.isValid()) continue;
            const auto it = last.find(write.texture.index);
            if (it == last.end())
                pass.barriers.push_back({write.texture, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, write.stage, 0,
                                         write.access, VK_IMAGE_LAYOUT_UNDEFINED, write.layout});
            else if (it->second.layout != write.layout)
                pass.barriers.push_back({write.texture, it->second.stage, write.stage, it->second.access, write.access,
                                         it->second.layout, write.layout});
            last[write.texture.index] = write;
        }
    }
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    const auto isDepth = [](VkFormat f) {
        return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM ||
               f == VK_FORMAT_D32_SFLOAT_S8_UINT;
    };
    for (const Pass& pass : m_passes) {
        const bool timed = m_passTimer && !pass.selfTimed;
        if (timed) m_passTimer->begin(cmd, pass.name);
        for (const Barrier& b : pass.barriers) {
            Texture& tex = m_textures[b.texture.index];
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            ib.srcAccessMask = b.srcAccess;
            ib.dstAccessMask = b.dstAccess;
            ib.oldLayout = b.oldLayout;
            ib.newLayout = b.newLayout;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = tex.image;
            // The aspect from the format: a depth texture read as SHADER_READ_ONLY_OPTIMAL is still depth.
            // Every layer moves: the graph tracks one layout per texture (a 4-cascade shadow array moved at
            // layer 0 only left cascades 1-3 in the attachment layout while lighting sampled them).
            ib.subresourceRange = {isDepth(tex.format) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                       : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                                   0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(cmd, b.srcStage, b.dstStage, 0, 0, nullptr, 0, nullptr, 1, &ib);
            tex.layout = b.newLayout;
        }
        if (pass.execute) pass.execute(cmd);
        if (timed) m_passTimer->end(cmd);
    }
}

void RenderGraph::reset() {
    m_passes.clear();
    // Every frame's first barrier on a texture then goes from UNDEFINED, valid whatever the image holds.
    for (Texture& t : m_textures) t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

} // namespace ohao
