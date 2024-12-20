#include "ohao_vk_pipeline.hpp"
#include "ohao_vk_device.hpp"
#include "ohao_vk_render_pass.hpp"
#include "ohao_vk_shader_module.hpp"
#include "core/asset/model.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>

namespace ohao {

OhaoVkPipeline::~OhaoVkPipeline() {
    cleanup();
}

void OhaoVkPipeline::cleanup() {
    if (device) {
        if (graphicsPipeline) {
            vkDestroyPipeline(device->getDevice(), graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        if (pipelineLayout) {
            vkDestroyPipelineLayout(device->getDevice(), pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
    }
}

bool OhaoVkPipeline::initialize(
    OhaoVkDevice* device,
    OhaoVkRenderPass* renderPass,
    OhaoVkShaderModule* shaderModule,
    VkExtent2D swapChainExtent,
    VkDescriptorSetLayout descriptorSetLayout,
    RenderMode mode,
    const PipelineConfigInfo* configInfo,
    VkPipelineLayout layout)
{
    this->device = device;
    this->renderPass = renderPass;
    this->shaderModule = shaderModule;
    this->extent = swapChainExtent;
    this->renderMode = mode;


    if (layout != VK_NULL_HANDLE) {
        pipelineLayout = layout;
        return createPipeline(mode, configInfo);
    } else {
        // Use push constants for selection pipeline
        bool success = (mode == RenderMode::WIREFRAME) ?
            createPipelineLayoutWithPushConstants(descriptorSetLayout) :
            createDefaultPipelineLayout(descriptorSetLayout);

        return success && createPipeline(mode, configInfo);
    }
}

void OhaoVkPipeline::bind(VkCommandBuffer commandBuffer) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
}

bool OhaoVkPipeline::createPipeline(RenderMode mode, const PipelineConfigInfo* configInfo) {

    // Get shader stages based on mode
    VkPipelineShaderStageCreateInfo vertShaderStageInfo;
    VkPipelineShaderStageCreateInfo fragShaderStageInfo;

    if (mode == RenderMode::GIZMO) {
        vertShaderStageInfo = shaderModule->getShaderStageInfo("gizmo_vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("gizmo_frag");
    } else if (mode == RenderMode::WIREFRAME ) {
        vertShaderStageInfo = shaderModule->getShaderStageInfo("selection_vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("selection_frag");
    } else {
        vertShaderStageInfo = shaderModule->getShaderStageInfo("vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("frag");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo, fragShaderStageInfo
    };

    // Use default config if none provided
    PipelineConfigInfo defaultConfig{};
    if (!configInfo) {
        defaultPipelineConfigInfo(defaultConfig, extent);
        configInfo = &defaultConfig;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = configInfo->inputAssemblyInfo;
    if (mode == RenderMode::GIZMO) {
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }

    // Vertex input state
    auto bindingDescription = Vertex::getBindingDescriptions();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
    vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &configInfo->inputAssemblyInfo;
    pipelineInfo.pViewportState = &configInfo->viewportInfo;
    pipelineInfo.pRasterizationState = &configInfo->rasterizationInfo;
    pipelineInfo.pMultisampleState = &configInfo->multisampleInfo;
    pipelineInfo.pColorBlendState = &configInfo->colorBlendInfo;
    pipelineInfo.pDepthStencilState = &configInfo->depthStencilInfo;
    pipelineInfo.pDynamicState = &configInfo->dynamicStateInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass->getVkRenderPass();
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(
        device->getDevice(),
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &graphicsPipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkPipeline::createPipelineLayoutWithPushConstants(VkDescriptorSetLayout descriptorSetLayout) {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(SelectionPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device->getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout with push constants!" << std::endl;
        return false;
    }
    return true;
}

bool OhaoVkPipeline::createDefaultPipelineLayout(VkDescriptorSetLayout descriptorSetLayout) {
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    if (vkCreatePipelineLayout(device->getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout!" << std::endl;
        return false;
    }
    return true;
}

void OhaoVkPipeline::defaultPipelineConfigInfo(PipelineConfigInfo& configInfo, VkExtent2D extent) {
    // Input assembly
    configInfo.inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    configInfo.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    configInfo.inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor
    configInfo.viewport = {
        0.0f, 0.0f,
        static_cast<float>(extent.width), static_cast<float>(extent.height),
        0.0f, 1.0f
    };

    configInfo.scissor = {{0, 0}, extent};

    configInfo.viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    configInfo.viewportInfo.viewportCount = 1;
    configInfo.viewportInfo.pViewports = &configInfo.viewport;
    configInfo.viewportInfo.scissorCount = 1;
    configInfo.viewportInfo.pScissors = &configInfo.scissor;

    // Rasterization
    configInfo.rasterizationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    configInfo.rasterizationInfo.depthClampEnable = VK_FALSE;
    configInfo.rasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
    configInfo.rasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL; // Default, modified for WIREFRAME/GIZMO
    configInfo.rasterizationInfo.lineWidth = 1.0f;
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    configInfo.rasterizationInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
    configInfo.rasterizationInfo.depthBiasEnable = VK_FALSE;

    // Multisampling
    configInfo.multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    configInfo.multisampleInfo.sampleShadingEnable = VK_FALSE;
    configInfo.multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blending
    configInfo.colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    configInfo.colorBlendAttachment.blendEnable = VK_FALSE;

    configInfo.colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    configInfo.colorBlendInfo.logicOpEnable = VK_FALSE;
    configInfo.colorBlendInfo.attachmentCount = 1;
    configInfo.colorBlendInfo.pAttachments = &configInfo.colorBlendAttachment;

    // Depth and stencil
    configInfo.depthStencilInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthWriteEnable = VK_TRUE;
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS;
    configInfo.depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
    configInfo.depthStencilInfo.stencilTestEnable = VK_FALSE;

    // Dynamic states
    configInfo.dynamicStateEnables.clear();
    configInfo.dynamicStateEnables = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    if (renderMode == RenderMode::GIZMO || renderMode == RenderMode::WIREFRAME) {
        configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
    }
    // Note: VK_DYNAMIC_STATE_LINE_WIDTH is added for GIZMO/WIREFRAME modes in createPipeline
    configInfo.dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(configInfo.dynamicStateEnables.size());
    configInfo.dynamicStateInfo.pDynamicStates = configInfo.dynamicStateEnables.data();
}

bool OhaoVkPipeline::createSelectionPipelineLayout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    VkPipelineLayout& pipelineLayout) {

    // Push constant range for selection shader
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(SelectionPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    return true;
}

} // namespace ohao
