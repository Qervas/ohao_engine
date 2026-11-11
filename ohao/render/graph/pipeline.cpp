#include "render/graph/pipeline.hpp"

#include "core/log.hpp"
#include "render/rt/rt_compute.hpp"

namespace ohao::render {

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& d) {
    VkShaderModule vert = d.vertex ? rtc::loadShaderModule(device, d.vertex) : VK_NULL_HANDLE;
    VkShaderModule frag = d.fragment ? rtc::loadShaderModule(device, d.fragment) : VK_NULL_HANDLE;
    const auto destroyModules = [&] {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device, vert, nullptr);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device, frag, nullptr);
    };
    if (vert == VK_NULL_HANDLE || (d.fragment && frag == VK_NULL_HANDLE)) {
        destroyModules();
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t stageCount = 0;
    stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stageCount].module = vert;
    stages[stageCount].pName = d.vertexEntry;
    stages[stageCount].pSpecializationInfo = d.vertexSpecialization;
    ++stageCount;
    if (frag != VK_NULL_HANDLE) {
        stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stageCount].module = frag;
        stages[stageCount].pName = d.fragmentEntry;
        stages[stageCount].pSpecializationInfo = d.fragmentSpecialization;
        ++stageCount;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(d.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = d.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(d.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = d.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = d.topology;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = d.polygonMode;
    raster.cullMode = d.cull;
    raster.frontFace = d.frontFace;
    raster.lineWidth = d.lineWidth;
    raster.depthClampEnable = d.depthClamp ? VK_TRUE : VK_FALSE;
    raster.depthBiasEnable = d.depthBias ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = d.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = d.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = d.depthCompare;
    depth.maxDepthBounds = 1.0f;

    std::vector<VkPipelineColorBlendAttachmentState> attachments(d.colorFormats.size());
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const Blend b = i < d.blends.size() ? d.blends[i] : Blend{};
        attachments[i].blendEnable = b.enable ? VK_TRUE : VK_FALSE;
        attachments[i].srcColorBlendFactor = b.srcColor;
        attachments[i].dstColorBlendFactor = b.dstColor;
        attachments[i].colorBlendOp = b.colorOp;
        attachments[i].srcAlphaBlendFactor = b.srcAlpha;
        attachments[i].dstAlphaBlendFactor = b.dstAlpha;
        attachments[i].alphaBlendOp = b.alphaOp;
        attachments[i].colorWriteMask = b.writeMask;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

    std::vector<VkDynamicState> dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (d.depthBias) dynamics.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    dynamics.insert(dynamics.end(), d.extraDynamic.begin(), d.extraDynamic.end());
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamics.size());
    dynamic.pDynamicStates = dynamics.data();

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<uint32_t>(d.colorFormats.size());
    rendering.pColorAttachmentFormats = d.colorFormats.data();
    rendering.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = d.layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    destroyModules();
    if (r != VK_SUCCESS) {
        OHAO_LOG(Error, render, "pipeline '{}': creation failed ({})", d.name, static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

} // namespace ohao::render
