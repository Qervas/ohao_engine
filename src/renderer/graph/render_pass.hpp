#pragma once

#include "resource_handle.hpp"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace ohao {

class RenderGraph;

/**
 * Pass builder for declaring resource dependencies.
 * Used during pass setup to specify which resources a pass reads/writes.
 */
class PassBuilder {
public:
    explicit PassBuilder(RenderGraph& graph, uint32_t passIndex);

    // Color attachment outputs
    TextureHandle createColorAttachment(const std::string& name, uint32_t width, uint32_t height,
                                         VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);

    TextureHandle createHdrColorAttachment(const std::string& name, uint32_t width, uint32_t height);

    // Depth attachment outputs
    TextureHandle createDepthAttachment(const std::string& name, uint32_t width, uint32_t height,
                                         VkFormat format = VK_FORMAT_D32_SFLOAT);

    // Shadow map output
    TextureHandle createShadowMap(const std::string& name, uint32_t size);

    // G-Buffer outputs for deferred rendering
    TextureHandle createGBufferAttachment(const std::string& name, uint32_t width, uint32_t height,
                                           VkFormat format);

    // Read a texture as shader input
    void readTexture(TextureHandle handle, VkPipelineStageFlags stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // Sample a texture in fragment shader
    void sampleTexture(TextureHandle handle);

    // Write to storage texture (compute)
    void writeStorageTexture(TextureHandle handle);

    // Use existing texture as color/depth attachment
    void useColorAttachment(TextureHandle handle);
    void useDepthAttachment(TextureHandle handle);

    // Buffer operations
    BufferHandle createBuffer(const std::string& name, VkDeviceSize size, BufferUsage usage);
    void readBuffer(BufferHandle handle, BufferUsage usage);
    void writeBuffer(BufferHandle handle);

    // Mark pass as compute-only (no render pass needed)
    void setComputeOnly();

    // Set viewport/scissor dimensions
    void setViewport(uint32_t width, uint32_t height);

private:
    friend class RenderGraph;

    RenderGraph& m_graph;
    uint32_t m_passIndex;
};

/**
 * Pass types
 */
enum class PassType {
    Graphics,    // Uses render pass with attachments
    Compute,     // Uses compute pipeline only
    Transfer     // Memory transfer operations only
};

/**
 * Render pass definition.
 * Contains all metadata about a pass including resource dependencies.
 */
struct RenderPassDef {
    std::string name;
    uint32_t index{0};
    PassType type{PassType::Graphics};

    // Resource accesses
    std::vector<ResourceAccess> reads;
    std::vector<ResourceAccess> writes;

    // Attachment info for graphics passes
    std::vector<TextureHandle> colorAttachments;
    TextureHandle depthAttachment{TextureHandle::invalid()};

    // Viewport dimensions
    uint32_t viewportWidth{0};
    uint32_t viewportHeight{0};

    // Compiled Vulkan resources (filled by graph compiler)
    VkRenderPass vulkanRenderPass{VK_NULL_HANDLE};
    VkFramebuffer vulkanFramebuffer{VK_NULL_HANDLE};

    // Execution callback
    std::function<void(VkCommandBuffer)> executeCallback;

    // Reference count for topological sorting
    uint32_t refCount{0};
    bool executed{false};
};

/**
 * Command buffer wrapper for pass execution.
 * Provides a cleaner API for recording commands within a pass.
 */
class PassCommandBuffer {
public:
    explicit PassCommandBuffer(VkCommandBuffer cmd, const RenderPassDef& pass)
        : m_cmd(cmd), m_pass(pass) {}

    VkCommandBuffer get() const { return m_cmd; }
    const RenderPassDef& getPass() const { return m_pass; }

    // Pipeline binding
    void bindPipeline(VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
        vkCmdBindPipeline(m_cmd, bindPoint, pipeline);
    }

    // Descriptor set binding
    void bindDescriptorSets(VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
                            uint32_t firstSet, uint32_t setCount,
                            const VkDescriptorSet* sets,
                            uint32_t dynamicOffsetCount = 0,
                            const uint32_t* dynamicOffsets = nullptr) {
        vkCmdBindDescriptorSets(m_cmd, bindPoint, layout, firstSet, setCount,
                                sets, dynamicOffsetCount, dynamicOffsets);
    }

    // Vertex/index buffer binding
    void bindVertexBuffers(uint32_t firstBinding, uint32_t bindingCount,
                           const VkBuffer* buffers, const VkDeviceSize* offsets) {
        vkCmdBindVertexBuffers(m_cmd, firstBinding, bindingCount, buffers, offsets);
    }

    void bindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
        vkCmdBindIndexBuffer(m_cmd, buffer, offset, indexType);
    }

    // Push constants
    void pushConstants(VkPipelineLayout layout, VkShaderStageFlags stageFlags,
                       uint32_t offset, uint32_t size, const void* values) {
        vkCmdPushConstants(m_cmd, layout, stageFlags, offset, size, values);
    }

    // Draw commands
    void draw(uint32_t vertexCount, uint32_t instanceCount = 1,
              uint32_t firstVertex = 0, uint32_t firstInstance = 0) {
        vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
                     uint32_t firstIndex = 0, int32_t vertexOffset = 0,
                     uint32_t firstInstance = 0) {
        vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void drawIndirect(VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
        vkCmdDrawIndirect(m_cmd, buffer, offset, drawCount, stride);
    }

    void drawIndexedIndirect(VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
        vkCmdDrawIndexedIndirect(m_cmd, buffer, offset, drawCount, stride);
    }

    // Compute dispatch
    void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1) {
        vkCmdDispatch(m_cmd, groupCountX, groupCountY, groupCountZ);
    }

    void dispatchIndirect(VkBuffer buffer, VkDeviceSize offset) {
        vkCmdDispatchIndirect(m_cmd, buffer, offset);
    }

    // Viewport/scissor (for dynamic state)
    void setViewport(float x, float y, float width, float height,
                     float minDepth = 0.0f, float maxDepth = 1.0f) {
        VkViewport viewport{x, y, width, height, minDepth, maxDepth};
        vkCmdSetViewport(m_cmd, 0, 1, &viewport);
    }

    void setScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
        VkRect2D scissor{{x, y}, {width, height}};
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
    }

private:
    VkCommandBuffer m_cmd;
    const RenderPassDef& m_pass;
};

} // namespace ohao
