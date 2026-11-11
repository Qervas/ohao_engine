#pragma once
// The one way a graphics pipeline is made (step 4a): for dynamic rendering, so it names its attachments' formats
// instead of a render pass. Viewport and scissor are dynamic (render::Pass sets both to the attachments' extent).
// Shader modules are loaded from bin/shaders/<file> and destroyed here.
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <vector>

namespace ohao::render {

struct Blend {
    bool enable{false};
    VkBlendFactor srcColor{VK_BLEND_FACTOR_ONE}, dstColor{VK_BLEND_FACTOR_ZERO};
    VkBlendFactor srcAlpha{VK_BLEND_FACTOR_ONE}, dstAlpha{VK_BLEND_FACTOR_ZERO};
    VkBlendOp colorOp{VK_BLEND_OP_ADD}, alphaOp{VK_BLEND_OP_ADD};
    VkColorComponentFlags writeMask{VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                    VK_COLOR_COMPONENT_A_BIT};
    [[nodiscard]] static Blend additive() {
        return {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE};
    }
    [[nodiscard]] static Blend alpha() {
        return {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    }
};

struct GraphicsPipelineDesc {
    const char* name{""};                  // for the log when creation fails
    const char* vertex{nullptr};           // SPIR-V file names under bin/shaders
    const char* fragment{nullptr};         // null: depth only
    const char* vertexEntry{"main"};
    const char* fragmentEntry{"main"};
    // A mesh pipeline (VK_EXT_mesh_shader) instead of a vertex one: `mesh`, with `task` or without; no vertex input.
    const char* task{nullptr};
    const char* mesh{nullptr};
    const char* taskEntry{"main"};
    const char* meshEntry{"main"};
    VkPipelineLayout layout{VK_NULL_HANDLE};

    // Attachments, as the pass that draws with it declares them. `blends` has one entry per colour format, or is
    // empty (no blending, every channel written).
    std::span<const VkFormat> colorFormats;
    std::span<const Blend> blends;
    VkFormat depthFormat{VK_FORMAT_UNDEFINED};

    // A fullscreen triangle by default: no vertex input, no culling, no depth.
    std::span<const VkVertexInputBindingDescription> vertexBindings;
    std::span<const VkVertexInputAttributeDescription> vertexAttributes;
    VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPolygonMode polygonMode{VK_POLYGON_MODE_FILL};
    VkCullModeFlags cull{VK_CULL_MODE_NONE};
    VkFrontFace frontFace{VK_FRONT_FACE_COUNTER_CLOCKWISE};
    float lineWidth{1.0f};
    bool depthTest{false};
    bool depthWrite{false};
    VkCompareOp depthCompare{VK_COMPARE_OP_GREATER_OR_EQUAL};
    bool depthClamp{false};
    bool depthBias{false};                 // constant and slope set dynamically (vkCmdSetDepthBias)
    std::span<const VkDynamicState> extraDynamic;   // besides viewport and scissor (and depth bias when on)
    const VkSpecializationInfo* fragmentSpecialization{nullptr};
    const VkSpecializationInfo* vertexSpecialization{nullptr};
};

// Null (logged) when a shader is missing or the driver refuses the pipeline.
[[nodiscard]] VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc);

} // namespace ohao::render
