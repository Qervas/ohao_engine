#pragma once

#include "render_pass_base.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"
#include "render/shader_bindings/overlay_gizmo.hpp"
#include "../gizmo/gizmo_meshes.hpp"
#include "render/scene/render_scene_types.hpp"

#include <span>

namespace ohao {

// Gizmo render pass - renders transform handles on top of the scene, and the frame's lines (render::DebugLine).
// Uses line list topology with depth test disabled (always visible).
// Simple vertex color shader (no lighting).
class GizmoPass : public RenderPassBase {
public:
    GizmoPass() = default;
    ~GizmoPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer, uint32_t) override {}   // record() draws: the pass needs its target
    // The handles composited over `target` (the final RGBA8 image) in place, timed as gizmo.
    void record(const render::Frame& frame, gpu::Image& target);
    [[nodiscard]] const char* getName() const override { return "GizmoPass"; }

    // The lines drawn with the handles, world space; their buffer is made again only when they change.
    void setLines(std::span<const render::DebugLine> lines);
    [[nodiscard]] bool hasLines() const noexcept { return !m_lines.empty(); }

    // Configure what to render
    void setEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] bool isEnabled() const { return m_enabled; }
    void setGizmoMode(GizmoMode mode);
    void setGizmoTransform(const glm::mat4& model);
    void setViewProjection(const glm::mat4& viewProj);
    void setHighlightedAxis(GizmoAxis axis) { m_highlightedAxis = axis; }
    [[nodiscard]] GizmoMode getGizmoMode() const { return m_currentMode; }

private:
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createVertexBuffers();
    void destroyVertexBuffers();
    void updateGizmoBuffers();

    bool m_enabled{false};
    GizmoMode m_currentMode{GizmoMode::TRANSLATE};
    GizmoAxis m_highlightedAxis{GizmoAxis::NONE};

    glm::mat4 m_viewProj{1.0f};
    glm::mat4 m_gizmoModel{1.0f};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Vertex/index buffers for gizmo geometry
    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
    uint32_t m_indexCount{0};

    // The lines: two vertices each.
    std::vector<GizmoVertex> m_lines;
    GpuBuffer m_lineBuffer;
    bool m_linesDirty{false};

    // Cached geometry
    std::vector<GizmoVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    bool m_geometryDirty{true};

    // Push constants (must match overlay/gizmo.slang)
    struct GizmoPushConstants {
        glm::mat4 viewProj;
        glm::mat4 model;
        glm::vec4 highlightColor;  // xyz = color, w = factor
    };
    OHAO_CHECK_PUSH(GizmoPushConstants, overlay_gizmo)   // overlay/gizmo.slang's
};

} // namespace ohao
