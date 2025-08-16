#include "scene_renderer.hpp"
#include "rhi/vk/ohao_vk_pipeline.hpp"
#include "scene_render_target.hpp"
#include "vulkan_context.hpp"
#include "ui/system/ui_manager.hpp"
#include "ui/components/console_widget.hpp"
#include "core/material/material.hpp"
#include "core/component/mesh_component.hpp"
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
    try {
        // First check if we have a valid context
        if (!context) {
            // Even without context, we should still reset our members
            selectionPipeline.reset();
            pipeline = nullptr;
            gizmoPipeline = nullptr;
            axisGizmo.reset();
            renderTarget.reset();
            selectionPipelineLayout = VK_NULL_HANDLE;
            isPipelineLayoutValid = false;
            return;
        }

        // Wait for device to be idle before cleanup
        try {
            context->getLogicalDevice()->waitIdle();
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to wait for device idle during cleanup: " + std::string(e.what()));
            // Continue with cleanup even if wait fails
        }

        // First destroy the pipeline that manages its own layout
        if (selectionPipeline) {
            try {
                selectionPipeline.reset();
            } catch (const std::exception& e) {
                OHAO_LOG_ERROR("Failed to reset selection pipeline: " + std::string(e.what()));
            }
        }

        // We no longer own the pipeline layout, so don't try to destroy it
        selectionPipelineLayout = VK_NULL_HANDLE;
        isPipelineLayoutValid = false;

        // Reset other resources
        try {
            pipeline = nullptr;
            gizmoPipeline = nullptr;
            axisGizmo.reset();
            renderTarget.reset();
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Failed to reset resources: " + std::string(e.what()));
        }

        // Clear the context pointer last
        context = nullptr;

    } catch (const std::exception& e) {
        // If anything goes wrong during cleanup, try to reset everything to a safe state
        OHAO_LOG_ERROR("Exception during cleanup: " + std::string(e.what()));
        selectionPipeline.reset();
        pipeline = nullptr;
        gizmoPipeline = nullptr;
        axisGizmo.reset();
        renderTarget.reset();
        selectionPipelineLayout = VK_NULL_HANDLE;
        isPipelineLayoutValid = false;
        context = nullptr;
    }
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

    // Clear values for color and depth - ensure depth is fully cleared to 1.0 (farthest)
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.1f, 0.1f, 0.1f, 1.0f}}; // Dark gray background
    clearValues[1].depthStencil = {1.0f, 0};           // Furthest depth (1.0)

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Configure viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(renderTarget->getWidth());
    viewport.height = static_cast<float>(renderTarget->getHeight());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {renderTarget->getWidth(), renderTarget->getHeight()};
    
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Store the command buffer for later use in render() and endFrame()
    currentCommandBuffer = cmd;
}

void SceneRenderer::render(OhaoVkUniformBuffer* uniformBuffer, uint32_t currentFrame) {
    if (!renderTarget || !currentCommandBuffer || !uniformBuffer) return;

    // Get the command buffer for this frame
    VkCommandBuffer cmd = currentCommandBuffer;

    // Bind the pipeline
    if (!pipeline) {
        OHAO_LOG_ERROR("Main pipeline not initialized");
        return;
    }
    pipeline->bind(cmd);

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

    // Bind descriptor set for this frame
    VkDescriptorSet descriptorSet = context->getDescriptor()->getSet(currentFrame);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline->getPipelineLayout(),
        0, 1, &descriptorSet,
        0, nullptr
    );

    // Collect actors that have mesh components and their buffer info
    std::vector<std::pair<Actor*, const MeshBufferInfo*>> sortedActors;
    auto* scene = context->getScene();
    if (scene) {
        for (const auto& [actorId, actor] : scene->getAllActors()) {
            // Skip actors without mesh components
            auto meshComponent = actor->getComponent<MeshComponent>();
            if (!meshComponent || !meshComponent->getModel()) continue;
            
            const auto* bufferInfo = context->getMeshBufferInfo(actor.get());
            if (bufferInfo && bufferInfo->indexCount > 0) {
                sortedActors.emplace_back(actor.get(), bufferInfo);
            }
        }
    }

    // If we have no actors to render, just draw the axis gizmo and return
    if (sortedActors.empty()) {
        if (axisGizmo && gizmoPipeline) {
            renderAxisGizmo(cmd, uniformBuffer, currentFrame);
        }
        return;
    }

    // Sort actors by Z position (near to far) for proper depth ordering
    std::sort(sortedActors.begin(), sortedActors.end(), 
              [](const auto& a, const auto& b) {
                 return a.first->getTransform()->getPosition().z > b.first->getTransform()->getPosition().z;
              });

    // Check if vertex and index buffers exist before using them
    auto vertexBufferPtr = context->getVertexBuffer();
    auto indexBufferPtr = context->getIndexBuffer();
    if (!vertexBufferPtr || !indexBufferPtr) {
        // If buffers are null but we have actors, try to update the scene buffers
        if (!sortedActors.empty()) {
            context->updateSceneBuffers();
            // Recheck buffers after update
            vertexBufferPtr = context->getVertexBuffer();
            indexBufferPtr = context->getIndexBuffer();
            if (!vertexBufferPtr || !indexBufferPtr) {
                OHAO_LOG_ERROR("Failed to create buffers for scene objects");
                return;
            }
        } else {
            // No actors and no buffers is fine, just return
            return;
        }
    }

    // Bind vertex and index buffers
    VkBuffer vertexBuffer = vertexBufferPtr->getBuffer();
    VkBuffer indexBuffer = indexBufferPtr->getBuffer();
    if (!vertexBuffer || !indexBuffer) {
        OHAO_LOG_ERROR("Vertex or index buffer handle is null in SceneRenderer::render");
        return;
    }

    VkBuffer vertexBuffers[] = {vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // First, draw all non-selected actors
    for (const auto& [actor, bufferInfo] : sortedActors) {
        if (SelectionManager::get().isSelected(actor)) continue; // Skip selected actors for now
        
        // Set model matrix and material properties as push constants
        OhaoVkPipeline::ModelPushConstants pushConstants{};
        pushConstants.model = actor->getTransform()->getWorldMatrix();
        
        // Get material properties from mesh component
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (meshComponent) {
            const auto& material = meshComponent->getMaterial();
            pushConstants.baseColor = material.baseColor;
            pushConstants.metallic = material.metallic;
            pushConstants.roughness = material.roughness;
            pushConstants.ao = material.ao;
        } else {
            // Default material if no mesh component
            pushConstants.baseColor = glm::vec3(0.8f, 0.8f, 0.8f);
            pushConstants.metallic = 0.0f;
            pushConstants.roughness = 0.5f;
            pushConstants.ao = 1.0f;
        }

        vkCmdPushConstants(
            cmd,
            pipeline->getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(OhaoVkPipeline::ModelPushConstants),
            &pushConstants
        );

        // Draw the model
        vkCmdDrawIndexed(
            cmd,
            bufferInfo->indexCount,
            1,
            bufferInfo->indexOffset,
            0,
            0
        );
    }
    
    // Then, draw all selected actors and their highlights
    for (const auto& [actor, bufferInfo] : sortedActors) {
        if (!SelectionManager::get().isSelected(actor)) continue; // Only process selected actors now
        
        // First draw the normal model
        OhaoVkPipeline::ModelPushConstants pushConstants{};
        pushConstants.model = actor->getTransform()->getWorldMatrix();
        
        // Get material properties from mesh component
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (meshComponent) {
            const auto& material = meshComponent->getMaterial();
            pushConstants.baseColor = material.baseColor;
            pushConstants.metallic = material.metallic;
            pushConstants.roughness = material.roughness;
            pushConstants.ao = material.ao;
        } else {
            // Default material if no mesh component
            pushConstants.baseColor = glm::vec3(0.8f, 0.8f, 0.8f);
            pushConstants.metallic = 0.0f;
            pushConstants.roughness = 0.5f;
            pushConstants.ao = 1.0f;
        }

        vkCmdPushConstants(
            cmd,
            pipeline->getPipelineLayout(),
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(OhaoVkPipeline::ModelPushConstants),
            &pushConstants
        );

        // Draw the model
        vkCmdDrawIndexed(
            cmd,
            bufferInfo->indexCount,
            1,
            bufferInfo->indexOffset,
            0,
            0
        );
        
        // Then draw the selection highlight
        drawSelectionHighlight(cmd, actor, *bufferInfo);
    }

    // Draw axis gizmo if available
    if (axisGizmo && gizmoPipeline) {
        renderAxisGizmo(cmd, uniformBuffer, currentFrame);
    }
}

void SceneRenderer::endFrame() {
    if (!renderTarget || !currentCommandBuffer) return;

    // End the render pass
    vkCmdEndRenderPass(currentCommandBuffer);

    // Reset the command buffer pointer for next frame
    currentCommandBuffer = VK_NULL_HANDLE;
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
    try {
        if (!context || !renderTarget) {
            OHAO_LOG_ERROR("Context or render target not available");
            return false;
        }

        // Ensure we don't have any existing pipeline
        selectionPipeline.reset();
        selectionPipelineLayout = VK_NULL_HANDLE;  // We will use the one owned by pipeline
        isPipelineLayoutValid = false;

        // Create pipeline configuration
        PipelineConfigInfo configInfo{};
        defaultSelectionPipelineConfig(configInfo,
            {renderTarget->getWidth(), renderTarget->getHeight()});

        // Create selection pipeline layout with same descriptor set layout as main pipeline
        VkDescriptorSetLayout descriptorSetLayout = context->getVkDescriptorSetLayout();
        if (descriptorSetLayout == VK_NULL_HANDLE) {
            OHAO_LOG_ERROR("Invalid descriptor set layout");
            return false;
        }

        // Set pipeline creation mode to WIREFRAME explicitly
        OhaoVkPipeline::RenderMode mode = OhaoVkPipeline::RenderMode::WIREFRAME;

        // Use the selection pipeline layout for the pipeline
        selectionPipeline = std::make_unique<OhaoVkPipeline>();
        if (!selectionPipeline->initialize(
                context->getLogicalDevice(),
                renderTarget->getRenderPass(),
                context->getShaderModules(),
                {renderTarget->getWidth(), renderTarget->getHeight()},
                descriptorSetLayout,
                mode,
                &configInfo)) { // Pass our config
            OHAO_LOG_ERROR("Failed to create selection pipeline");
            return false;
        }

        // Store a reference to the pipeline's layout but don't own it
        selectionPipelineLayout = selectionPipeline->getPipelineLayout();
        isPipelineLayoutValid = true;  // It's valid but we don't own it

        OHAO_LOG("Selection pipeline initialized successfully");
        return true;
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Exception during pipeline initialization: " + std::string(e.what()));
        selectionPipeline.reset();
        selectionPipelineLayout = VK_NULL_HANDLE;
        isPipelineLayoutValid = false;
        return false;
    }
}

void SceneRenderer::drawSelectionHighlight(VkCommandBuffer cmd, Actor* actor, const MeshBufferInfo& bufferInfo) {
    if (!selectionPipeline || !selectionPipelineLayout) {
        OHAO_LOG_WARNING("Selection pipeline or layout not initialized");
        return;
    }
    
    // Bind the selection pipeline
    selectionPipeline->bind(cmd);
    
    // Set line width
    vkCmdSetLineWidth(cmd, 1.5f);
    
    // Set a tiny depth bias dynamically
    vkCmdSetDepthBias(cmd, -0.0001f, 0.0f, 0.0f);
    
    // Get descriptor set and bind it
    auto descriptorSet = context->getDescriptor()->getSet(context->getCurrentFrame());
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        selectionPipelineLayout,
        0, 1, &descriptorSet,
        0, nullptr
    );
    
    // Use the exact model matrix with NO scaling at all
    struct CombinedPushConstants {
        OhaoVkPipeline::ModelPushConstants model;
        OhaoVkPipeline::SelectionPushConstants selection;
    } combinedConstants;
    
    // Use the exact model matrix - no scaling whatsoever
    combinedConstants.model.model = actor->getTransform()->getWorldMatrix();
    
    // Set selection parameters
    combinedConstants.selection.highlightColor = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
    combinedConstants.selection.scaleOffset = 0.0f;  // No offset at all
    
    // Push constants
    vkCmdPushConstants(
        cmd,
        selectionPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(CombinedPushConstants),
        &combinedConstants
    );
    
    // Draw the model edges
    vkCmdDrawIndexed(
        cmd,
        bufferInfo.indexCount,
        1,
        bufferInfo.indexOffset,
        0,
        0
    );
}

void SceneRenderer::defaultSelectionPipelineConfig(PipelineConfigInfo& configInfo, VkExtent2D extent) {
    // Create a temporary pipeline to access the static method
    OhaoVkPipeline tempPipeline;
    tempPipeline.defaultPipelineConfigInfo(configInfo, extent);
    
    // Configure for edge-only selection outlining
    configInfo.rasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;     // Use wireframe mode
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;           // Don't cull any faces
    configInfo.rasterizationInfo.lineWidth = 1.5f;                       // Moderate line width
    
    // Ensure wireframe is visible without any offset
    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthWriteEnable = VK_FALSE;
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    
    // Enable depth bias but we'll set it dynamically
    configInfo.rasterizationInfo.depthBiasEnable = VK_TRUE;
    
    // Enable the dynamic states
    configInfo.dynamicStateEnables.clear();
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_VIEWPORT);
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_SCISSOR);
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    
    // Update dynamic state info
    configInfo.dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(configInfo.dynamicStateEnables.size());
    configInfo.dynamicStateInfo.pDynamicStates = configInfo.dynamicStateEnables.data();
    
    // Disable blending for solid edge color
    configInfo.colorBlendAttachment.blendEnable = VK_FALSE;
}

void SceneRenderer::renderAxisGizmo(VkCommandBuffer cmd, OhaoVkUniformBuffer* uniformBuffer, uint32_t currentFrame) {
    if (!axisGizmo || !gizmoPipeline) return;
    
    // Bind the gizmo pipeline
    gizmoPipeline->bind(cmd);
    
    // Set line width for gizmo rendering
    vkCmdSetLineWidth(cmd, 2.0f);
    
    // Get descriptor from context
    auto descriptorSet = context->getDescriptor()->getSet(currentFrame);
    
    // Bind descriptor sets with gizmo pipeline layout
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        gizmoPipeline->getPipelineLayout(),
        0, 1,
        &descriptorSet,
        0, nullptr
    );
    
    // Get vertex and index buffers from the axis gizmo
    VkBuffer gizmoVertexBuffer = axisGizmo->getVertexBuffer();
    VkBuffer gizmoIndexBuffer = axisGizmo->getIndexBuffer();
    
    if (gizmoVertexBuffer != VK_NULL_HANDLE && gizmoIndexBuffer != VK_NULL_HANDLE) {
        // Bind the buffers
        VkBuffer vertexBuffers[] = {gizmoVertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, gizmoIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        
        // Get combined view-projection matrix from cached UBO
        const auto& ubo = uniformBuffer->getCachedUBO();
        glm::mat4 viewProj = ubo.proj * ubo.view;
        
        // Render the gizmo
        axisGizmo->render(cmd, viewProj);
    }
}

} // namespace ohao
