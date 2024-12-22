#include "scene_renderer.hpp"
#include "rhi/vk/ohao_vk_pipeline.hpp"
#include "scene_render_target.hpp"
#include "vulkan_context.hpp"
#include "ui/system/ui_manager.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>

namespace ohao {

SceneRenderer::~SceneRenderer() {
    cleanup();
}

bool SceneRenderer::initialize(VulkanContext* contextPtr) {
    context = contextPtr;
    if (!context) {
        std::cerr << "SceneRenderer: Invalid VulkanContext provided" << std::endl;
        return false;
    }

    uint32_t initialWidth = 800;
    uint32_t initialHeight = 600;

    if (context->getUIManager()) {
        auto viewportSize = context->getUIManager()->getSceneViewportSize();
        initialWidth = static_cast<uint32_t>(viewportSize.width);
        initialHeight = static_cast<uint32_t>(viewportSize.height);
    }

    // Initialize render target first
    renderTarget = std::make_unique<SceneRenderTarget>();
    if (!renderTarget->initialize(context, initialWidth, initialHeight)) {
        std::cerr << "SceneRenderer: Failed to initialize render target" << std::endl;
        return false;
    }

    // Initialize axis gizmo
    axisGizmo = std::make_unique<AxisGizmo>();
    if(!axisGizmo->initialize(context)){
        std::cerr << "SceneRenderer: Failed to initialize axis gizmo" << std::endl;
        return false;
    }

    // Initialize selection pipeline last, after render target is ready
    if (!initializeSelectionPipeline()) {
        std::cerr << "SceneRenderer: Failed to initialize selection pipeline" << std::endl;
        return false;
    }

    return true;
}

bool SceneRenderer::initializeRenderTarget(uint32_t width, uint32_t height) {
    return createRenderResources(width, height);
}

void SceneRenderer::cleanup() {
    if (context) {
        context->getLogicalDevice()->waitIdle();
        if (selectionPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(context->getVkDevice(), selectionPipelineLayout, nullptr);
            selectionPipelineLayout = VK_NULL_HANDLE;
        }
        selectionPipeline.reset();
    }
    pipeline = nullptr;
    gizmoPipeline = nullptr;
    selectionPipeline.reset();
    axisGizmo.reset();
    renderTarget.reset();
}

bool SceneRenderer::createRenderResources(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }

    renderTarget = std::make_unique<SceneRenderTarget>();
    return renderTarget->initialize(context, width, height);
}

void SceneRenderer::beginFrame() {
    if (!renderTarget) return;

    VkCommandBuffer cmd = context->getCommandManager()->getCommandBuffer(context->getCurrentFrame());

    // Begin render pass for scene
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderTarget->getVkRenderPass();
    renderPassInfo.framebuffer = renderTarget->getFramebuffer();
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {
        renderTarget->getWidth(),
        renderTarget->getHeight()
    };

    // Clear values for color and depth
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.2f, 0.2f, 0.2f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(renderTarget->getWidth());
    viewport.height = static_cast<float>(renderTarget->getHeight());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {renderTarget->getWidth(), renderTarget->getHeight()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void SceneRenderer::render(OhaoVkUniformBuffer* uniformBuffer, uint32_t currentFrame) {
    if (!renderTarget) return;
    VkCommandBuffer cmd = context->getCommandManager()->getCommandBuffer(context->getCurrentFrame());
    beginFrame();
    // Draw the scene if we have one and valid buffers
    if (context->hasLoadScene() && pipeline) {
        VkBuffer vertexBuffer = context->getVkVertexBuffer();
        VkBuffer indexBuffer = context->getVkIndexBuffer();

        if (vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE) {
            // First bind the pipeline
            pipeline->bind(cmd);

            // Then bind descriptor sets with the correct pipeline layout
            auto descriptorSet = context->getDescriptor()->getSet(currentFrame);
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->getPipelineLayout(),
                0, 1,
                &descriptorSet,
                0, nullptr
            );

            // Bind buffers
            VkBuffer vertexBuffers[] = {vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            // Draw each object
            auto scene = context->getScene();
            if (scene) {
                for (const auto& [name, object] : scene->getObjects()) {
                    if (object && object->getModel()) {
                        auto bufferInfo = context->getMeshBufferInfo(object.get());
                        if (bufferInfo) {
                            // Draw object
                            vkCmdDrawIndexed(cmd,
                                bufferInfo->indexCount,
                                1,
                                bufferInfo->indexOffset,
                                bufferInfo->vertexOffset,
                                0);

                            // Draw selection highlight if selected
                            if (SelectionManager::get().isSelected(object.get())) {
                                // Bind descriptor sets again with selection pipeline layout
                                vkCmdBindDescriptorSets(
                                    cmd,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    selectionPipeline->getPipelineLayout(),
                                    0, 1,
                                    &descriptorSet,
                                    0, nullptr
                                );
                                drawSelectionHighlight(cmd, object.get(), *bufferInfo);
                            }
                        }
                    }
                }
            }
        }
    }

    // Draw the axis gizmo if it has valid buffers
    if (axisGizmo && gizmoPipeline) {
        gizmoPipeline->bind(cmd);

        // Bind descriptor sets with gizmo pipeline layout
        auto descriptorSet = context->getDescriptor()->getSet(currentFrame);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            gizmoPipeline->getPipelineLayout(),
            0, 1,
            &descriptorSet,
            0, nullptr
        );
        VkBuffer gizmoVertexBuffer = axisGizmo->getVertexBuffer();
        VkBuffer gizmoIndexBuffer = axisGizmo->getIndexBuffer();

        if (gizmoVertexBuffer != VK_NULL_HANDLE && gizmoIndexBuffer != VK_NULL_HANDLE) {
            gizmoPipeline->bind(cmd);
            if (gizmoPipeline->getRenderMode() == OhaoVkPipeline::RenderMode::GIZMO) {
                vkCmdSetLineWidth(cmd, 2.0f);
            }

            VkBuffer vertexBuffers[] = {gizmoVertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, gizmoIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

            const auto& ubo = uniformBuffer->getCachedUBO();
            glm::mat4 viewProj = ubo.proj * ubo.view;
            axisGizmo->render(cmd, viewProj);
        }
    }

    endFrame();
}

void SceneRenderer::endFrame() {
    if (!renderTarget) return;

    VkCommandBuffer cmd = context->getCommandManager()->getCommandBuffer(context->getCurrentFrame());
    vkCmdEndRenderPass(cmd);
}

OhaoVkTextureHandle SceneRenderer::getViewportTexture() const {
    VkDescriptorSet descriptorSet = renderTarget ? renderTarget->getDescriptorSet() : VK_NULL_HANDLE;
    return OhaoVkTextureHandle(descriptorSet);
}

void SceneRenderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;

    // Wait for device to be idle
    if (context) {
        context->getLogicalDevice()->waitIdle();
    }

    try {
        if (renderTarget) {
            renderTarget->resize(width, height);
        } else {
            renderTarget = std::make_unique<SceneRenderTarget>();
            if (!renderTarget->initialize(context, width, height)) {
                throw std::runtime_error("Failed to initialize render target");
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to resize scene renderer: " << e.what() << std::endl;
        // If resize fails, try to restore a valid state
        if (renderTarget) {
            renderTarget->cleanup();
            renderTarget = nullptr;
        }
    }
}

ViewportSize SceneRenderer::getViewportSize() const {
    if (renderTarget) {
        return {renderTarget->getWidth(), renderTarget->getHeight()};
    }
    return {0, 0};
}

bool SceneRenderer::hasValidRenderTarget() const{
    return renderTarget->hasValidRenderTarget();
}

bool SceneRenderer::initializeSelectionPipeline() {
    if (!context || !renderTarget) {
        OHAO_LOG_ERROR("Context or render target not available");
        return false;
    }

    if (renderTarget->getWidth() == 0 || renderTarget->getHeight() == 0) {
        OHAO_LOG_ERROR("Invalid render target dimensions");
        return false;
    }
    selectionPipeline = std::make_unique<OhaoVkPipeline>();

    // Create pipeline configuration
    PipelineConfigInfo configInfo{};
    selectionPipeline->defaultPipelineConfigInfo(configInfo,
        {renderTarget->getWidth(), renderTarget->getHeight()});

    // Configure pipeline settings
    configInfo.rasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;
    configInfo.rasterizationInfo.lineWidth = 2.0f;
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;

    configInfo.colorBlendAttachment.blendEnable = VK_TRUE;
    configInfo.colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    configInfo.colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    configInfo.colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    configInfo.colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    configInfo.colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthWriteEnable = VK_FALSE;
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);

    // Create selection pipeline
    selectionPipeline = std::make_unique<OhaoVkPipeline>();
    bool success = selectionPipeline->initialize(
        context->getLogicalDevice(),
        renderTarget->getRenderPass(),
        context->getShaderModules(),
        context->getSwapChain()->getExtent(),
        context->getVkDescriptorSetLayout(),
        OhaoVkPipeline::RenderMode::WIREFRAME,
        &configInfo
    );

    if (!success) {
        OHAO_LOG_ERROR("Failed to create selection pipeline");
        return false;
    }

    return true;
}

void SceneRenderer::drawSelectionHighlight(VkCommandBuffer cmd,
                                         SceneObject* object,
                                         const MeshBufferInfo& bufferInfo) {
    if (!selectionPipeline) return;

    // Set selection pipeline
    selectionPipeline->bind(cmd);

    // Set line width for outline
    vkCmdSetLineWidth(cmd, 2.0f);

    // Push constants for highlight effect
    OhaoVkPipeline::SelectionPushConstants pushConstants{
        glm::vec4(1.0f, 0.5f, 0.0f, 1.0f),  // Orange highlight
        0.02f  // Scale offset
    };

    vkCmdPushConstants(
        cmd,
        selectionPipeline->getPipelineLayout(),
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(OhaoVkPipeline::SelectionPushConstants),
        &pushConstants
    );

    // Draw the selection outline
    vkCmdDrawIndexed(
        cmd,
        bufferInfo.indexCount,
        1,
        bufferInfo.indexOffset,
        bufferInfo.vertexOffset,
        0
    );
}

} // namespace ohao
