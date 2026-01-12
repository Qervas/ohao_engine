#include "subsystems/shadow/shadow_renderer.hpp"
#include "subsystems/shadow/shadow_map_render_target.hpp"
#include "vulkan_context.hpp"
#include "rhi/vk/ohao_vk_pipeline.hpp"
#include "rhi/vk/ohao_vk_shader_module.hpp"
#include "rhi/vk/ohao_vk_uniform_buffer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <unordered_set>

namespace ohao {

ShadowRenderer::~ShadowRenderer() {
    cleanup();
}

bool ShadowRenderer::initialize(VulkanContext* ctx) {
    context = ctx;

    // Create shadow map render target
    shadowMapTarget = std::make_unique<ShadowMapRenderTarget>();
    if (!shadowMapTarget->initialize(context)) {
        std::cerr << "Failed to initialize shadow map render target" << std::endl;
        return false;
    }

    // Create shadow uniform buffer
    if (!createShadowUniformBuffer()) {
        std::cerr << "Failed to create shadow uniform buffer" << std::endl;
        cleanup();
        return false;
    }

    // Create shadow pipeline
    if (!createShadowPipeline()) {
        std::cerr << "Failed to create shadow pipeline" << std::endl;
        cleanup();
        return false;
    }

    std::cout << "Shadow renderer initialized successfully" << std::endl;
    return true;
}

void ShadowRenderer::cleanup() {
    if (!context) return;

    context->getLogicalDevice()->waitIdle();

    shadowPipeline.reset();
    shadowShaderModule.reset();
    shadowUniformBuffer.reset();
    shadowMapTarget.reset();
}

bool ShadowRenderer::createShadowUniformBuffer() {
    shadowUniformBuffer = std::make_unique<OhaoVkUniformBuffer>();
    if (!shadowUniformBuffer->initialize(context->getLogicalDevice(), 2, sizeof(ShadowUniformBuffer))) {
        return false;
    }
    return true;
}

bool ShadowRenderer::createShadowPipeline() {
    // Create shader module for shadow pass
    shadowShaderModule = std::make_unique<OhaoVkShaderModule>();
    if (!shadowShaderModule->initialize(context->getLogicalDevice())) {
        std::cerr << "Failed to initialize shadow shader module" << std::endl;
        return false;
    }

    // Load shadow depth shaders
    if (!shadowShaderModule->createShaderModule(
            "shadow_vert", "shaders/shadow_depth.vert.spv",
            OhaoVkShaderModule::ShaderType::VERTEX) ||
        !shadowShaderModule->createShaderModule(
            "shadow_frag", "shaders/shadow_depth.frag.spv",
            OhaoVkShaderModule::ShaderType::FRAGMENT)) {
        std::cerr << "Failed to load shadow depth shaders" << std::endl;
        return false;
    }

    // Create pipeline config for depth-only rendering
    PipelineConfigInfo configInfo;

    // Input assembly
    configInfo.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    configInfo.inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (will be set dynamically)
    configInfo.viewport.x = 0.0f;
    configInfo.viewport.y = 0.0f;
    configInfo.viewport.width = static_cast<float>(shadowMapTarget->getWidth());
    configInfo.viewport.height = static_cast<float>(shadowMapTarget->getHeight());
    configInfo.viewport.minDepth = 0.0f;
    configInfo.viewport.maxDepth = 1.0f;

    configInfo.scissor.offset = {0, 0};
    configInfo.scissor.extent = {shadowMapTarget->getWidth(), shadowMapTarget->getHeight()};

    configInfo.viewportInfo.viewportCount = 1;
    configInfo.viewportInfo.pViewports = &configInfo.viewport;
    configInfo.viewportInfo.scissorCount = 1;
    configInfo.viewportInfo.pScissors = &configInfo.scissor;

    // Rasterization - enable depth bias to prevent shadow acne
    configInfo.rasterizationInfo.depthClampEnable = VK_FALSE;
    configInfo.rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
    configInfo.rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
    configInfo.rasterizationInfo.lineWidth = 1.0f;
    // Disable culling for shadow mapping - ensures ALL faces contribute to shadow
    // This fixes holes in cube shadows and works for all light directions
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    configInfo.rasterizationInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    configInfo.rasterizationInfo.depthBiasEnable = VK_TRUE;
    configInfo.rasterizationInfo.depthBiasConstantFactor = 1.25f;
    configInfo.rasterizationInfo.depthBiasSlopeFactor = 1.75f;
    configInfo.rasterizationInfo.depthBiasClamp = 0.0f;

    // Multisampling
    configInfo.multisampleInfo.sampleShadingEnable = VK_FALSE;
    configInfo.multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No color blending for depth-only pass
    configInfo.colorBlendInfo.attachmentCount = 0;
    configInfo.colorBlendInfo.pAttachments = nullptr;

    // Depth stencil
    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthWriteEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    configInfo.depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
    configInfo.depthStencilInfo.stencilTestEnable = VK_FALSE;

    // Dynamic state
    configInfo.dynamicStateEnables = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    configInfo.dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(configInfo.dynamicStateEnables.size());
    configInfo.dynamicStateInfo.pDynamicStates = configInfo.dynamicStateEnables.data();

    // Create shadow pipeline
    shadowPipeline = std::make_unique<OhaoVkPipeline>();
    VkExtent2D extent = {shadowMapTarget->getWidth(), shadowMapTarget->getHeight()};

    if (!shadowPipeline->initialize(
            context->getLogicalDevice(),
            shadowMapTarget->getVkRenderPass(),  // Use raw VkRenderPass overload
            shadowShaderModule.get(),
            extent,
            context->getVkDescriptorSetLayout(),
            OhaoVkPipeline::RenderMode::SHADOW,
            &configInfo)) {
        std::cerr << "Failed to create shadow pipeline" << std::endl;
        return false;
    }

    return true;
}

void ShadowRenderer::beginShadowPass(VkCommandBuffer cmd) {
    if (!enabled || !shadowMapTarget) return;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = shadowMapTarget->getVkRenderPass();
    renderPassInfo.framebuffer = shadowMapTarget->getFramebuffer();
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {shadowMapTarget->getWidth(), shadowMapTarget->getHeight()};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(shadowMapTarget->getWidth());
    viewport.height = static_cast<float>(shadowMapTarget->getHeight());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {shadowMapTarget->getWidth(), shadowMapTarget->getHeight()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind shadow pipeline
    if (shadowPipeline) {
        shadowPipeline->bind(cmd);
    }
}

void ShadowRenderer::renderShadowMap(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!enabled || !context || !shadowPipeline) return;

    auto scene = context->getScene();
    if (!scene) return;

    // Bind vertex and index buffers
    VkBuffer vertexBuffer = context->getVkVertexBuffer();
    VkBuffer indexBuffer = context->getVkIndexBuffer();

    if (vertexBuffer == VK_NULL_HANDLE || indexBuffer == VK_NULL_HANDLE) {
        static bool warned = false;
        if (!warned) {
            std::cout << "[Shadow] WARNING: Vertex or index buffer is NULL!" << std::endl;
            warned = true;
        }
        return;
    }

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Bind descriptor set for shadow uniform buffer
    VkDescriptorSet descriptorSet = context->getDescriptor()->getSet(frameIndex);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           shadowPipeline->getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    // Render all meshes to shadow map
    static bool debugOnce = false;
    int renderCount = 0;
    for (const auto& [actorId, actor] : scene->getAllActors()) {
        if (!actor) continue;

        // Skip actors hidden in editor (eye icon toggle)
        if (!actor->isEditorVisible()) continue;

        auto meshComponent = actor->getComponent<MeshComponent>();
        if (!meshComponent || !meshComponent->isVisible()) continue;

        MeshBufferInfo bufferInfo;
        if (!context->getMeshBufferInfo(actor->getID(), bufferInfo)) {
            // Debug: log lookup failure
            static std::unordered_set<uint64_t> loggedFailures;
            if (loggedFailures.find(actor->getID()) == loggedFailures.end()) {
                std::cerr << "[Shadow] WARNING: Actor '" << actor->getName()
                          << "' (ID=" << actor->getID() << ") not found in meshBufferMap!" << std::endl;
                loggedFailures.insert(actor->getID());
            }
            continue;
        }
        if (bufferInfo.indexCount == 0) continue;

        renderCount++;

        // Get model matrix from transform
        auto transform = actor->getComponent<TransformComponent>();
        glm::mat4 modelMatrix = transform ? transform->getWorldMatrix() : glm::mat4(1.0f);

        // Get material properties
        auto materialComp = actor->getComponent<MaterialComponent>();
        glm::vec3 baseColor = materialComp ? materialComp->getMaterial().baseColor : glm::vec3(0.8f);
        float metallic = materialComp ? materialComp->getMaterial().metallic : 0.0f;
        float roughness = materialComp ? materialComp->getMaterial().roughness : 0.5f;
        float ao = materialComp ? materialComp->getMaterial().ao : 1.0f;

        // Push constants
        OhaoVkPipeline::ModelPushConstants pushConstants{};
        pushConstants.model = modelMatrix;
        pushConstants.baseColor = baseColor;
        pushConstants.metallic = metallic;
        pushConstants.roughness = roughness;
        pushConstants.ao = ao;

        vkCmdPushConstants(cmd, shadowPipeline->getPipelineLayout(),
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(OhaoVkPipeline::ModelPushConstants), &pushConstants);

        // Note: indices in buffer are already offset by vertexOffset during buffer building
        // So we pass 0 for vertexOffset here to avoid double-offsetting
        vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }

    // Always show render count for debugging multi-object shadows
    static int frameCount = 0;
    frameCount++;
    if (frameCount % 300 == 1) { // Every ~5 seconds at 60fps
        std::cout << "[Shadow] Frame " << std::hex << frameCount << std::dec
                  << ": Rendered " << renderCount
                  << "/" << scene->getAllActors().size() << " objects to shadow map"
                  << " | orthoSize=" << orthoSize << std::endl;

        // Debug: List which objects were rendered
        std::cout << "[Shadow] Objects rendered:" << std::endl;
        for (const auto& [actorId, actor] : scene->getAllActors()) {
            if (!actor) continue;
            auto meshComp = actor->getComponent<MeshComponent>();
            bool hasMesh = meshComp && meshComp->isVisible();
            bool inMap = (context->getMeshBufferInfo(static_cast<uint64_t>(actor->getID())) != nullptr);

            std::cout << "  - " << actor->getName()
                      << " | hasMesh=" << hasMesh
                      << " | inBufferMap=" << inMap;
            if (hasMesh && inMap) {
                auto transform = actor->getTransform();
                glm::vec3 pos(0.0f);
                if (transform) {
                    pos = transform->getPosition();
                    std::cout << " | pos=(" << pos.x << "," << pos.y << "," << pos.z << ")";
                }
                // Show buffer offsets and light-space Z
                const auto* bufInfo = context->getMeshBufferInfo(static_cast<uint64_t>(actor->getID()));
                if (bufInfo) {
                    std::cout << " | iCnt=" << bufInfo->indexCount;
                }
                glm::vec4 lsPos = lightSpaceMatrix * glm::vec4(pos, 1.0f);
                glm::vec3 ndc = glm::vec3(lsPos) / lsPos.w;
                std::cout << " | lsZ=" << ndc.z;
                if (ndc.z < 0.0f || ndc.z > 1.0f) {
                    std::cout << " [DEPTH OUT OF RANGE!]";
                }
            }
            std::cout << std::endl;
        }
    }
}

void ShadowRenderer::endShadowPass(VkCommandBuffer cmd) {
    if (!enabled) return;
    vkCmdEndRenderPass(cmd);
}

glm::mat4 ShadowRenderer::calculateLightSpaceMatrix(const UnifiedLight& light, const glm::vec3& sceneCenter) const {
    // Handle different light types
    if (light.isSpot() || light.isPoint()) {
        return calculateSpotLightSpaceMatrix(light);
    }

    // Directional light: orthographic projection
    // light.direction points FROM the light, so we negate it to get the direction TO the light
    glm::vec3 lightDir = glm::normalize(light.direction);
    glm::vec3 lightPos = sceneCenter - lightDir * farPlane * 0.5f;

    // Calculate up vector (avoid gimbal lock when light points straight up/down)
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, up);

    // Create Vulkan-compatible orthographic projection
    glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);

    // Convert from OpenGL Z range [-1, 1] to Vulkan Z range [0, 1]
    lightProj[2][2] = -1.0f / (farPlane - nearPlane);
    lightProj[3][2] = -nearPlane / (farPlane - nearPlane);

    // Vulkan clip space has Y pointing down, need to flip Y in projection
    lightProj[1][1] *= -1.0f;

    return lightProj * lightView;
}

glm::mat4 ShadowRenderer::calculateSpotLightSpaceMatrix(const UnifiedLight& light) const {
    // Spot/Point light: perspective projection from light's position
    glm::vec3 lightPos = light.position;
    glm::vec3 lightDir = glm::normalize(light.direction);

    // For point lights without direction, default to looking down
    if (light.isPoint()) {
        lightDir = glm::vec3(0.0f, -1.0f, 0.0f);
    }

    // Calculate up vector (avoid gimbal lock)
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    // View matrix: look from light position in light direction
    glm::vec3 target = lightPos + lightDir;
    glm::mat4 lightView = glm::lookAt(lightPos, target, up);

    // Perspective projection
    // For spot lights, use outer cone angle as FOV
    // For point lights, use 90 degrees (would need cubemap for full coverage)
    float fov = light.isSpot() ? glm::radians(light.outerCone * 2.0f) : glm::radians(90.0f);
    fov = glm::clamp(fov, glm::radians(10.0f), glm::radians(170.0f)); // Clamp to reasonable range

    float aspect = 1.0f; // Shadow map is square
    float shadowNear = 0.1f;
    float shadowFar = light.range > 0.0f ? light.range : farPlane;

    glm::mat4 lightProj = glm::perspective(fov, aspect, shadowNear, shadowFar);

    // Convert from OpenGL Z range [-1, 1] to Vulkan Z range [0, 1]
    lightProj[2][2] = shadowFar / (shadowNear - shadowFar);
    lightProj[3][2] = -(shadowFar * shadowNear) / (shadowFar - shadowNear);

    // Vulkan clip space has Y pointing down
    lightProj[1][1] *= -1.0f;

    return lightProj * lightView;
}

void ShadowRenderer::updateShadowUniforms(uint32_t frameIndex, const glm::mat4& matrix) {
    lightSpaceMatrix = matrix;

    if (shadowUniformBuffer) {
        ShadowUniformBuffer shadowUBO{};
        shadowUBO.lightSpaceMatrix = lightSpaceMatrix;
        shadowUniformBuffer->writeToBuffer(frameIndex, &shadowUBO, sizeof(ShadowUniformBuffer));
    }
}

VkImageView ShadowRenderer::getShadowMapImageView() const {
    return shadowMapTarget ? shadowMapTarget->getDepthImageView() : VK_NULL_HANDLE;
}

VkSampler ShadowRenderer::getShadowMapSampler() const {
    return shadowMapTarget ? shadowMapTarget->getShadowSampler() : VK_NULL_HANDLE;
}

} // namespace ohao
