#include "gizmo_pass.hpp"
#include "render/graph/pipeline.hpp"
#include <array>
#include <span>
#include "core/log.hpp"

namespace ohao {

namespace {
constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;   // the final image's
} // namespace

GizmoPass::~GizmoPass() {
    cleanup();
}

bool GizmoPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    if (!m_allocator) return false;
    m_device = device;
    m_physicalDevice = physicalDevice;

    // Generate initial geometry
    GizmoMeshes::generateTranslationGizmo(m_vertices, m_indices);
    m_geometryDirty = true;

    if (!createPipeline()) {
        OHAO_LOG(Error, render, "GizmoPass: Failed to create pipeline");
        return false;
    }
    if (!createVertexBuffers()) {
        OHAO_LOG(Error, render, "GizmoPass: Failed to create vertex buffers");
        return false;
    }

    return true;
}

void GizmoPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    destroyVertexBuffers();

    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
}

void GizmoPass::setGizmoMode(GizmoMode mode) {
    if (m_currentMode == mode) return;
    m_currentMode = mode;

    switch (mode) {
        case GizmoMode::TRANSLATE:
            GizmoMeshes::generateTranslationGizmo(m_vertices, m_indices);
            break;
        case GizmoMode::ROTATE:
            GizmoMeshes::generateRotationGizmo(m_vertices, m_indices);
            break;
        case GizmoMode::SCALE:
            GizmoMeshes::generateScaleGizmo(m_vertices, m_indices);
            break;
    }
    m_geometryDirty = true;
}

void GizmoPass::setGizmoTransform(const glm::mat4& model) {
    m_gizmoModel = model;
}

void GizmoPass::setViewProjection(const glm::mat4& viewProj) {
    m_viewProj = viewProj;
}

void GizmoPass::record(const render::Frame& frame, gpu::Image& target) {
    if (!m_enabled || m_pipeline == VK_NULL_HANDLE || !target || target.desc.format != kTargetFormat) return;

    // Update geometry buffers if needed
    if (m_geometryDirty) {
        updateGizmoBuffers();
    }

    if (m_indexCount == 0 || !m_vertexBuffer.isValid() || !m_indexBuffer.isValid()) return;

    render::Pass pass(frame, "gizmo", render::Pass::Raster);
    pass.read(m_vertexBuffer, render::BufferUse::Vertex).read(m_indexBuffer, render::BufferUse::Index);
    pass.color(target, render::Load::Keep);   // composited over what is there
    pass.begin();
    const VkCommandBuffer cmd = frame.cmd;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkBuffer vertexBuffers[] = {m_vertexBuffer.buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Push constants
    GizmoPushConstants pc{};
    pc.viewProj = m_viewProj;
    pc.model = m_gizmoModel;
    pc.highlightColor = glm::vec4(GizmoMeshes::HIGHLIGHT_COLOR, 0.0f);

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(GizmoPushConstants), &pc);

    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

bool GizmoPass::createPipeline() {
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(GizmoPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    // Vertex input: GizmoVertex = position(vec3) + color(vec3)
    const VkVertexInputBindingDescription binding{0, sizeof(GizmoVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GizmoVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GizmoVertex, color)},
    }};
    const render::Blend blend{true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE,
                              VK_BLEND_FACTOR_ZERO};

    // No depth: gizmos always render on top.
    render::GraphicsPipelineDesc desc{};
    desc.name = "gizmo";
    desc.vertex = "overlay_gizmo.slang.vertexMain.spv";
    desc.fragment = "overlay_gizmo.slang.fragmentMain.spv";
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.layout = m_pipelineLayout;
    desc.colorFormats = {&kTargetFormat, 1};
    desc.blends = {&blend, 1};
    desc.vertexBindings = {&binding, 1};
    desc.vertexAttributes = attributes;
    desc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    desc.lineWidth = 2.0f;
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

bool GizmoPass::createVertexBuffers() {
    if (m_vertices.empty() || m_indices.empty()) return true;

    m_vertexBuffer = m_allocator->createBufferFromBytes(std::as_bytes(std::span(m_vertices)),
                                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_indexBuffer = m_allocator->createBufferFromBytes(std::as_bytes(std::span(m_indices)),
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!m_vertexBuffer.isValid() || !m_indexBuffer.isValid()) return false;

    m_indexCount = static_cast<uint32_t>(m_indices.size());
    m_geometryDirty = false;

    return true;
}

// The buffers a frame in flight may still draw from are retired, not destroyed.
void GizmoPass::destroyVertexBuffers() {
    m_allocator->retire(m_vertexBuffer);
    m_allocator->retire(m_indexBuffer);
    m_indexCount = 0;
}

void GizmoPass::updateGizmoBuffers() {
    destroyVertexBuffers();
    createVertexBuffers();
}

} // namespace ohao
