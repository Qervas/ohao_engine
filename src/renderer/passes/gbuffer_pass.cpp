#include "gbuffer_pass.hpp"
#include "../../engine/scene/scene.hpp"
#include "../../engine/actor/actor.hpp"
#include "../../engine/component/transform_component.hpp"
#include "../../renderer/components/mesh_component.hpp"
#include "../../renderer/components/material_component.hpp"
#include <stdexcept>
#include <array>

namespace ohao {

GBufferPass::~GBufferPass() {
    cleanup();
}

bool GBufferPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    // Default size - will be resized
    m_width = 1920;
    m_height = 1080;

    if (!createGBuffer()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffer()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void GBufferPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_gbufferLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_gbufferLayout, nullptr);
        m_gbufferLayout = VK_NULL_HANDLE;
    }
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    destroyGBuffer();
}

void GBufferPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_scene) return;

    // Begin render pass
    std::array<VkClearValue, GBUFFER_COUNT> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Position
    clearValues[1].color = {{0.5f, 0.5f, 1.0f, 0.0f}}; // Normal (up = 0.5, 0.5, 1.0)
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // Albedo
    clearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Velocity
    clearValues[4].depthStencil = {1.0f, 0};           // Depth

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Bind vertex and index buffers if available
    if (m_vertexBuffer != VK_NULL_HANDLE && m_indexBuffer != VK_NULL_HANDLE) {
        VkBuffer vertexBuffers[] = {m_vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    }

    // Iterate through scene actors and render meshes
    if (m_meshBufferMap) {
        const auto& actors = m_scene->getAllActors();
        for (const auto& [actorId, actor] : actors) {
            if (!actor) continue;

            // Get mesh component
            auto meshComp = actor->getComponent<MeshComponent>();
            if (!meshComp || !meshComp->isVisible()) continue;

            // Get transform
            auto transformComp = actor->getComponent<TransformComponent>();
            glm::mat4 modelMatrix = transformComp ? transformComp->getWorldMatrix() : glm::mat4(1.0f);

            // Get material (optional) - extract properties from Material struct
            auto materialComp = actor->getComponent<MaterialComponent>();
            glm::vec3 albedo = materialComp ? materialComp->getMaterial().baseColor : glm::vec3(0.8f);
            float metallic = materialComp ? materialComp->getMaterial().metallic : 0.0f;
            float roughness = materialComp ? materialComp->getMaterial().roughness : 0.5f;

            // Find buffer info for this actor
            auto it = m_meshBufferMap->find(actorId);
            if (it == m_meshBufferMap->end()) continue;

            const MeshBufferInfo& bufferInfo = it->second;

            // Get AO from material if available
            float ao = materialComp ? materialComp->getMaterial().ao : 1.0f;

            // Set push constants with transform and material data
            GBufferUBO ubo{};
            ubo.model = modelMatrix;
            ubo.view = m_view;
            ubo.projection = m_projection;
            ubo.prevMVP = m_prevViewProj * modelMatrix;
            ubo.materialParams = glm::vec4(metallic, roughness, ao, 0.0f);
            ubo.albedoColor = glm::vec4(albedo, 1.0f);

            vkCmdPushConstants(cmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(GBufferUBO), &ubo);

            // Draw indexed
            vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1,
                            bufferInfo.indexOffset, bufferInfo.vertexOffset, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

void GBufferPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    // Recreate framebuffer
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    }

    destroyGBuffer();
    createGBuffer();
    createFramebuffer();
    createDescriptors();
}

void GBufferPass::setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                                    const glm::mat4& prevViewProj) {
    m_view = view;
    m_projection = proj;
    m_prevViewProj = prevViewProj;
}

void GBufferPass::setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer) {
    m_vertexBuffer = vertexBuffer;
    m_indexBuffer = indexBuffer;
}

bool GBufferPass::createGBuffer() {
    // G-Buffer formats:
    // 0: Position + Metallic - RGBA16F
    // 1: Normal + Roughness - RGB10A2
    // 2: Albedo + AO - RGBA8 SRGB
    // 3: Velocity - RG16F
    // 4: Depth - D32F

    struct GBufferFormat {
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
    };

    std::array<GBufferFormat, GBUFFER_COUNT> formats = {{
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT}
    }};

    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        auto& target = m_gbuffer[i];
        target.format = formats[i].format;
        target.width = m_width;
        target.height = m_height;

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = formats[i].format;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = formats[i].usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &target.image) != VK_SUCCESS) {
            return false;
        }

        // Allocate memory
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, target.image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &target.memory) != VK_SUCCESS) {
            return false;
        }

        vkBindImageMemory(m_device, target.image, target.memory, 0);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = formats[i].format;
        viewInfo.subresourceRange.aspectMask = formats[i].aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &target.view) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void GBufferPass::destroyGBuffer() {
    for (auto& target : m_gbuffer) {
        target.destroy(m_device);
    }
}

bool GBufferPass::createRenderPass() {
    // Attachment descriptions
    std::array<VkAttachmentDescription, GBUFFER_COUNT> attachments{};

    // Position + Metallic
    attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Normal + Roughness
    attachments[1].format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Albedo + AO
    attachments[2].format = VK_FORMAT_R8G8B8A8_SRGB;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Velocity
    attachments[3].format = VK_FORMAT_R16G16_SFLOAT;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth
    attachments[4].format = VK_FORMAT_D32_SFLOAT;
    attachments[4].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[4].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[4].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // Subpass
    std::array<VkAttachmentReference, 4> colorRefs{};
    for (uint32_t i = 0; i < 4; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkAttachmentReference depthRef{};
    depthRef.attachment = 4;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;

    // Subpass dependencies
    std::array<VkSubpassDependency, 2> dependencies{};

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    return vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool GBufferPass::createFramebuffer() {
    std::array<VkImageView, GBUFFER_COUNT> attachments;
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        attachments[i] = m_gbuffer[i].view;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    return vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

bool GBufferPass::createDescriptors() {
    // Create sampler for G-Buffer textures
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (m_sampler == VK_NULL_HANDLE) {
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return false;
        }
    }

    // Descriptor set layout for G-Buffer access
    std::array<VkDescriptorSetLayoutBinding, GBUFFER_COUNT> bindings{};
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (m_gbufferLayout == VK_NULL_HANDLE) {
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_gbufferLayout) != VK_SUCCESS) {
            return false;
        }
    }

    // Descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = GBUFFER_COUNT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_gbufferLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_gbufferDescriptor) != VK_SUCCESS) {
        return false;
    }

    // Update descriptor set
    std::array<VkDescriptorImageInfo, GBUFFER_COUNT> imageInfos{};
    std::array<VkWriteDescriptorSet, GBUFFER_COUNT> writes{};

    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        imageInfos[i].sampler = m_sampler;
        imageInfos[i].imageView = m_gbuffer[i].view;
        imageInfos[i].imageLayout = (i == 4) ?
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_gbufferDescriptor;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    return true;
}

bool GBufferPass::createPipeline() {
    // Load shaders
    VkShaderModule vertShader = loadShaderModule("core_gbuffer.vert.spv");
    VkShaderModule fragShader = loadShaderModule("core_gbuffer.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShader;
    shaderStages[1].pName = "main";

    // Vertex input - matching model.hpp Vertex struct
    // Layout: position(vec3), color(vec3), normal(vec3), texCoord(vec2)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(float) * 11; // pos(3) + color(3) + normal(3) + uv(2)
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributeDescs{};
    // Position
    attributeDescs[0].binding = 0;
    attributeDescs[0].location = 0;
    attributeDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[0].offset = 0;
    // Color
    attributeDescs[1].binding = 0;
    attributeDescs[1].location = 1;
    attributeDescs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[1].offset = sizeof(float) * 3;
    // Normal
    attributeDescs[2].binding = 0;
    attributeDescs[2].location = 2;
    attributeDescs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescs[2].offset = sizeof(float) * 6;
    // TexCoord
    attributeDescs[3].binding = 0;
    attributeDescs[3].location = 3;
    attributeDescs[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescs[3].offset = sizeof(float) * 9;

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending (no blending for G-Buffer)
    std::array<VkPipelineColorBlendAttachmentState, 4> colorBlendAttachments{};
    for (auto& attachment : colorBlendAttachments) {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.data();

    // Dynamic state
    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Push constants for per-object data
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(GBufferUBO);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0; // Per-object textures would go here
    pipelineLayoutInfo.pSetLayouts = nullptr;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}

} // namespace ohao
