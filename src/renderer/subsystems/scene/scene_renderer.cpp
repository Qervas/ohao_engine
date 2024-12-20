#include "scene_renderer.hpp"
#include "scene_render_target.hpp"
#include "vulkan_context.hpp"
#include "ui/system/ui_manager.hpp"
#include <iostream>

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
    axisGizmo = std::make_unique<AxisGizmo>();
    if(!axisGizmo->initialize(context)){
        std::cerr << "SceneRenderer: Failed to initialize axis gizmo" << std::endl;
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
    }
    pipeline = nullptr;
    gizmoPipeline = nullptr;
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
    // Always bind descriptor sets first
    auto descriptorSet = context->getDescriptor()->getSet(currentFrame);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline->getPipelineLayout(),
        0, 1,
        &descriptorSet,
        0, nullptr
    );

    // Draw the scene if we have one and valid buffers
    if (context->hasLoadScene() && pipeline) {
        VkBuffer vertexBuffer = context->getVkVertexBuffer();
        VkBuffer indexBuffer = context->getVkIndexBuffer();

        if (vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE) {
            pipeline->bind(cmd);

            // Update light properties before drawing scene
            auto scene = context->getScene();
            if (scene && !scene->getLights().empty()) {
                const auto& light = scene->getLights().begin()->second;
                uniformBuffer->setLightProperties(
                    light.position,
                    light.color,
                    light.intensity
                );
            }

            // Bind vertex and index buffers for the scene
            VkBuffer vertexBuffers[] = {vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            // Draw the scene
            if (scene) {
                auto sceneObjects = scene->getObjects();
                if (!sceneObjects.empty()) {
                    auto mainObject = sceneObjects.begin()->second;
                    if (mainObject && mainObject->getModel()) {
                        vkCmdDrawIndexed(
                            cmd,
                            static_cast<uint32_t>(mainObject->getModel()->indices.size()),
                            1, 0, 0, 0
                        );
                    }
                }
            }
        }
    }

    // Draw the axis gizmo if it has valid buffers
    if (axisGizmo && gizmoPipeline) {
        VkBuffer gizmoVertexBuffer = axisGizmo->getVertexBuffer();
        VkBuffer gizmoIndexBuffer = axisGizmo->getIndexBuffer();

        if (gizmoVertexBuffer != VK_NULL_HANDLE && gizmoIndexBuffer != VK_NULL_HANDLE) {
            gizmoPipeline->bind(cmd);
            vkCmdSetLineWidth(cmd, 2.0f);

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
} // namespace ohao
