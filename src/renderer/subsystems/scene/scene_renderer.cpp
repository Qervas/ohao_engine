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
    return true;
}

bool SceneRenderer::initializeRenderTarget(uint32_t width, uint32_t height) {
    return createRenderResources(width, height);
}

void SceneRenderer::cleanup() {
    if (context) {
        context->getLogicalDevice()->waitIdle();
    }
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
    renderPassInfo.renderPass = renderTarget->getRenderPass();
    renderPassInfo.framebuffer = renderTarget->getFramebuffer();
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {
        renderTarget->getWidth(),
        renderTarget->getHeight()
    };

    // Clear values for color and depth
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
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
    if (!renderTarget || !context->hasLoadScene()) return;

    VkCommandBuffer cmd = context->getCommandManager()->getCommandBuffer(context->getCurrentFrame());

    // Bind the scene rendering pipeline
    context->getPipeline()->bind(cmd);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {context->getVkVertexBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, context->getVkIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);

    // Bind descriptor sets
    auto descriptorSet = context->getDescriptor()->getSet(currentFrame);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        context->getVkPipelineLayout(),
        0, 1,
        &descriptorSet,
        0, nullptr
    );

    // Draw the scene
    auto scene = context->getScene();
    if (scene) {
        auto sceneObjects = scene->getObjects();
        if (!sceneObjects.empty()) {
            auto mainObject = sceneObjects.begin()->second;
            vkCmdDrawIndexed(
                cmd,
                static_cast<uint32_t>(mainObject->model->indices.size()),
                1, 0, 0, 0
            );
        }
    }
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
