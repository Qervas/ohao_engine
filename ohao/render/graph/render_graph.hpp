#pragma once
// The deferred frame's pass list (system design 4.6: the frame graph that replaces it is step 4a). Passes run in
// the order added; each declares which imported textures it reads and writes, and the graph puts the layout
// transitions between them and times each pass. It allocates nothing, aliases nothing and culls nothing.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::render { class GpuPassTimer; }

namespace ohao {

struct TextureHandle {
    uint32_t index{std::numeric_limits<uint32_t>::max()};
    [[nodiscard]] constexpr bool isValid() const noexcept { return index != std::numeric_limits<uint32_t>::max(); }
    [[nodiscard]] constexpr bool operator==(const TextureHandle&) const noexcept = default;
    [[nodiscard]] static constexpr TextureHandle invalid() noexcept { return {}; }
};

class RenderGraph;

// Declares what a pass does to the graph's textures.
class PassBuilder {
public:
    PassBuilder(RenderGraph& graph, uint32_t passIndex) : m_graph(graph), m_passIndex(passIndex) {}
    // Sampled (SHADER_READ_ONLY_OPTIMAL) at `stage`.
    void readTexture(TextureHandle handle, VkPipelineStageFlags stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    void readComputeTexture(TextureHandle handle) { readTexture(handle, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); }
    // Written as an attachment by the pass's own render pass, which leaves it in `finalLayout`.
    void declareColorWrite(TextureHandle handle, VkImageLayout finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    void declareDepthWrite(TextureHandle handle,
                           VkImageLayout finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

private:
    RenderGraph& m_graph;
    uint32_t m_passIndex;
};

class RenderGraph {
public:
    void initialize(VkDevice device) { m_device = device; }
    void shutdown();   // forgets every texture and pass
    [[nodiscard]] bool isInitialized() const { return m_device != VK_NULL_HANDLE; }

    // `selfTimed`: the pass times its own parts under their registry names (one open timer at a time).
    void addPass(std::string_view name, std::function<void(PassBuilder&)> setup,
                 std::function<void(VkCommandBuffer)> execute, bool selfTimed = false);
    void addComputePass(std::string_view name, std::function<void(PassBuilder&)> setup,
                        std::function<void(VkCommandBuffer)> execute) {
        addPass(name, std::move(setup), std::move(execute));
    }

    // A texture the renderer owns, in `currentLayout` (each frame starts it from UNDEFINED: reset()).
    [[nodiscard]] TextureHandle importTexture(std::string_view name, VkImage image, VkImageView view, VkFormat format,
                                              uint32_t width, uint32_t height,
                                              VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    // Works out the barriers between the passes, in the order they were added.
    void compile();
    // Records every pass with its barriers, each timed under its name (a render/frame/pass_registry.hpp name).
    void execute(VkCommandBuffer cmd);
    void setPassTimer(render::GpuPassTimer* timer) noexcept { m_passTimer = timer; }
    // Clears the passes for the next frame; the textures stay, their layouts back to UNDEFINED.
    void reset();

private:
    friend class PassBuilder;

    struct Access {
        TextureHandle texture;
        bool write{false};
        VkPipelineStageFlags stage{0};
        VkAccessFlags access{0};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
    struct Texture {
        VkImage image{VK_NULL_HANDLE};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
    struct Barrier {
        TextureHandle texture;
        VkPipelineStageFlags srcStage{0}, dstStage{0};
        VkAccessFlags srcAccess{0}, dstAccess{0};
        VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED}, newLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
    struct Pass {
        std::string name;
        bool selfTimed{false};
        std::vector<Access> reads, writes;
        std::vector<Barrier> barriers;   // before the pass (compile)
        std::function<void(VkCommandBuffer)> execute;
    };

    VkDevice m_device{VK_NULL_HANDLE};
    std::vector<Texture> m_textures;
    std::vector<Pass> m_passes;
    render::GpuPassTimer* m_passTimer{nullptr};
};

} // namespace ohao
