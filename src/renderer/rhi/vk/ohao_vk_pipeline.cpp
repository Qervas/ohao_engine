#include "ohao_vk_pipeline.hpp"
#include "ohao_vk_device.hpp"
#include "ohao_vk_render_pass.hpp"
#include "ohao_vk_shader_module.hpp"
#include "engine/asset/model.hpp"
#include <iostream>
#include <vulkan/vulkan_core.h>
#include <algorithm>

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
    this->rawRenderPass = VK_NULL_HANDLE;  // Use wrapper
    this->shaderModule = shaderModule;
    this->extent = swapChainExtent;
    this->renderMode = mode;


    if (layout != VK_NULL_HANDLE) {
        pipelineLayout = layout;
        return createPipeline(mode, configInfo);
    } else {
        // Choose appropriate pipeline layout creation based on mode
        bool success = false;

        if (mode == RenderMode::WIREFRAME) {
            success = createPipelineLayoutWithPushConstants(descriptorSetLayout);
        } else if (mode == RenderMode::PUSH_CONSTANT_MODEL || mode == RenderMode::SHADOW) {
            success = createModelPushConstantPipelineLayout(descriptorSetLayout);
        } else {
            success = createDefaultPipelineLayout(descriptorSetLayout);
        }

        return success && createPipeline(mode, configInfo);
    }
}

bool OhaoVkPipeline::initialize(
    OhaoVkDevice* device,
    VkRenderPass rawRenderPass,
    OhaoVkShaderModule* shaderModule,
    VkExtent2D swapChainExtent,
    VkDescriptorSetLayout descriptorSetLayout,
    RenderMode mode,
    const PipelineConfigInfo* configInfo,
    VkPipelineLayout layout)
{
    this->device = device;
    this->renderPass = nullptr;  // Not using wrapper
    this->rawRenderPass = rawRenderPass;
    this->shaderModule = shaderModule;
    this->extent = swapChainExtent;
    this->renderMode = mode;

    if (layout != VK_NULL_HANDLE) {
        pipelineLayout = layout;
        return createPipeline(mode, configInfo);
    } else {
        // Choose appropriate pipeline layout creation based on mode
        bool success = false;

        if (mode == RenderMode::WIREFRAME) {
            success = createPipelineLayoutWithPushConstants(descriptorSetLayout);
        } else if (mode == RenderMode::PUSH_CONSTANT_MODEL || mode == RenderMode::SHADOW) {
            success = createModelPushConstantPipelineLayout(descriptorSetLayout);
        } else {
            success = createDefaultPipelineLayout(descriptorSetLayout);
        }

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
    } else if (mode == RenderMode::WIREFRAME) {
        // Use regular shaders for wireframe rendering, not selection shaders
        vertShaderStageInfo = shaderModule->getShaderStageInfo("vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("frag");
    } else if (mode == RenderMode::SHADOW) {
        // Use shadow depth shaders
        vertShaderStageInfo = shaderModule->getShaderStageInfo("shadow_vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("shadow_frag");
    } else {
        // Both SOLID and PUSH_CONSTANT_MODEL use the same shaders
        vertShaderStageInfo = shaderModule->getShaderStageInfo("vert");
        fragShaderStageInfo = shaderModule->getShaderStageInfo("frag");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo, fragShaderStageInfo
    };

    // Handle pipeline configuration
    PipelineConfigInfo defaultConfig{};
    PipelineConfigInfo localConfig{};  // Local copy of the config we'll use

    if (!configInfo) {
        defaultPipelineConfigInfo(defaultConfig, extent);
        localConfig = defaultConfig;
    } else {
        localConfig = *configInfo;  // Make a local copy of the provided config
    }

    // Modify input assembly based on mode
    if (mode == RenderMode::GIZMO) {
        localConfig.inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    } else if (mode == RenderMode::WIREFRAME) {
        // For wireframe mode, use line drawing with proper line width
        localConfig.rasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;
        localConfig.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
        localConfig.rasterizationInfo.lineWidth = 1.0f; // Authentic wireframe line width
    }

    // Update dynamic states based on mode
    if (mode == RenderMode::GIZMO || mode == RenderMode::WIREFRAME) {
        if (std::find(localConfig.dynamicStateEnables.begin(),
                      localConfig.dynamicStateEnables.end(),
                      VK_DYNAMIC_STATE_LINE_WIDTH) == localConfig.dynamicStateEnables.end()) {
            localConfig.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
        }
    }

    // Update dynamic state info to point to our local vector
    localConfig.dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(localConfig.dynamicStateEnables.size());
    localConfig.dynamicStateInfo.pDynamicStates = localConfig.dynamicStateEnables.data();

    // Vertex input state
    auto bindingDescription = Vertex::getBindingDescriptions();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.pNext = nullptr;
    vertexInputInfo.flags = 0;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescription.size());
    vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = nullptr;
    pipelineInfo.flags = 0;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &localConfig.inputAssemblyInfo;
    pipelineInfo.pViewportState = &localConfig.viewportInfo;
    pipelineInfo.pRasterizationState = &localConfig.rasterizationInfo;
    pipelineInfo.pMultisampleState = &localConfig.multisampleInfo;
    pipelineInfo.pColorBlendState = &localConfig.colorBlendInfo;
    pipelineInfo.pDepthStencilState = &localConfig.depthStencilInfo;
    pipelineInfo.pDynamicState = &localConfig.dynamicStateInfo;
    pipelineInfo.layout = pipelineLayout;
    // Use raw render pass if available, otherwise get from wrapper
    pipelineInfo.renderPass = (rawRenderPass != VK_NULL_HANDLE) ? rawRenderPass : renderPass->getVkRenderPass();
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(
        device->getDevice(),
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &graphicsPipeline) != VK_SUCCESS)
    {
        std::cerr << "Failed to create graphics pipeline!" << std::endl;
        return false;
    }

    return true;
}

bool OhaoVkPipeline::createPipelineLayoutWithPushConstants(VkDescriptorSetLayout descriptorSetLayout) {
    // Set up a single combined push constant range that covers both model and selection data
    VkPushConstantRange pushConstantRange{};
    
    // Combined push constants for vertex and fragment shaders
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ModelPushConstants) + sizeof(SelectionPushConstants);

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
    // Add push constant range for model matrix since the main shader now requires it
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ModelPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device->getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout!" << std::endl;
        return false;
    }
    return true;
}

bool OhaoVkPipeline::createModelPushConstantPipelineLayout(VkDescriptorSetLayout descriptorSetLayout) {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ModelPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device->getDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout with model push constants!" << std::endl;
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
    
    // Disable back-face culling for now to see all faces of the cube
    configInfo.rasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    
    // Set front face to clockwise which matches our model data
    configInfo.rasterizationInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
    
    configInfo.rasterizationInfo.depthBiasEnable = VK_FALSE;
    configInfo.rasterizationInfo.depthBiasConstantFactor = 0.0f;
    configInfo.rasterizationInfo.depthBiasClamp = 0.0f;
    configInfo.rasterizationInfo.depthBiasSlopeFactor = 0.0f;

    // Multisampling
    configInfo.multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    configInfo.multisampleInfo.sampleShadingEnable = VK_FALSE;
    configInfo.multisampleInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    configInfo.multisampleInfo.minSampleShading = 1.0f;
    configInfo.multisampleInfo.pSampleMask = nullptr;
    configInfo.multisampleInfo.alphaToCoverageEnable = VK_FALSE;
    configInfo.multisampleInfo.alphaToOneEnable = VK_FALSE;

    // Color blending
    configInfo.colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    configInfo.colorBlendAttachment.blendEnable = VK_FALSE;
    configInfo.colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    configInfo.colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    configInfo.colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    configInfo.colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    configInfo.colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    configInfo.colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    configInfo.colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    configInfo.colorBlendInfo.logicOpEnable = VK_FALSE;
    configInfo.colorBlendInfo.logicOp = VK_LOGIC_OP_COPY;
    configInfo.colorBlendInfo.attachmentCount = 1;
    configInfo.colorBlendInfo.pAttachments = &configInfo.colorBlendAttachment;
    configInfo.colorBlendInfo.blendConstants[0] = 0.0f;
    configInfo.colorBlendInfo.blendConstants[1] = 0.0f;
    configInfo.colorBlendInfo.blendConstants[2] = 0.0f;
    configInfo.colorBlendInfo.blendConstants[3] = 0.0f;

    // Depth and stencil - critical for correct 3D rendering
    configInfo.depthStencilInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    configInfo.depthStencilInfo.depthTestEnable = VK_TRUE;       // Enable depth testing
    configInfo.depthStencilInfo.depthWriteEnable = VK_TRUE;      // Write to depth buffer
    configInfo.depthStencilInfo.depthCompareOp = VK_COMPARE_OP_LESS; // Standard depth test
    configInfo.depthStencilInfo.depthBoundsTestEnable = VK_FALSE;
    configInfo.depthStencilInfo.minDepthBounds = 0.0f;
    configInfo.depthStencilInfo.maxDepthBounds = 1.0f;
    configInfo.depthStencilInfo.stencilTestEnable = VK_FALSE;
    configInfo.depthStencilInfo.front = {};  // Optional
    configInfo.depthStencilInfo.back = {};   // Optional

    // Dynamic states
    configInfo.dynamicStateEnables.clear();
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_VIEWPORT);
    configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_SCISSOR);

    if (renderMode == RenderMode::GIZMO || renderMode == RenderMode::WIREFRAME) {
        configInfo.dynamicStateEnables.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
    }
    // Update dynamic state info
    configInfo.dynamicStateInfo = {};
    configInfo.dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    configInfo.dynamicStateInfo.pNext = nullptr;
    configInfo.dynamicStateInfo.flags = 0;
    configInfo.dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(configInfo.dynamicStateEnables.size());
    configInfo.dynamicStateInfo.pDynamicStates = configInfo.dynamicStateEnables.data();

    // Double check that the pointer is valid
    if (configInfo.dynamicStateEnables.size() > 0) {
        assert(configInfo.dynamicStateInfo.pDynamicStates != nullptr);
    }
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
