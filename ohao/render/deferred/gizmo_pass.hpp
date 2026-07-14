#pragma once

#include "render_pass_base.hpp"
#include "../gizmo/gizmo_meshes.hpp"

namespace ohao {

// Gizmo render pass - renders transform handles on top of the scene.
// Uses line list topology with depth test disabled (always visible).
// Simple vertex color shader (no lighting).
class GizmoPass : public RenderPassBase {
public:
    GizmoPass() = default;
    ~GizmoPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "GizmoPass"; }

    // Configure what to render
    void setEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return m_enabled; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoTransform(const glm::mat4& model);
    void setViewProjection(const glm::mat4& viewProj);
    void setHighlightedAxis(GizmoAxis axis) { m_highlightedAxis = axis; }
    [[nodiscard]] GizmoMode getGizmoMode() const { return m_currentMode; }

    // Set the target image to composite gizmos onto.
    // The gizmo pass will create its own framebuffer from this image view.
    void setTargetImage(VkImage image, VkImageView imageView);

private:
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createVertexBuffers();
    void destroyVertexBuffers();
    void updateGizmoBuffers();
    void destroyFramebuffer();

    bool m_enabled{false};
    GizmoMode m_currentMode{GizmoMode::TRANSLATE};
    GizmoAxis m_highlightedAxis{GizmoAxis::NONE};

    glm::mat4 m_viewProj{1.0f};
    glm::mat4 m_gizmoModel{1.0f};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Own render pass (LOAD_OP_LOAD for compositing on top)
    VkRenderPass m_renderPass{VK_NULL_HANDLE};

    // Target image (from overlay or post-processing pass)
    VkImage m_targetImage{VK_NULL_HANDLE};
    VkImageView m_targetImageView{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Vertex/index buffers for gizmo geometry
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_vertexMemory{VK_NULL_HANDLE};
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indexMemory{VK_NULL_HANDLE};
    uint32_t m_indexCount{0};

    // Cached geometry
    std::vector<GizmoVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    bool m_geometryDirty{true};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constants (must match gizmo.vert)
    struct GizmoPushConstants {
        glm::mat4 viewProj;
        glm::mat4 model;
        glm::vec4 highlightColor;  // xyz = color, w = factor
    };
};

} // namespace ohao
