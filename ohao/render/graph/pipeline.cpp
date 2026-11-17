#include "render/graph/pipeline.hpp"

#include "core/log.hpp"
#include "render/rt/rt_compute.hpp"

namespace ohao::render {

VkPipeline createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& d) {
    // The geometry stages (a vertex shader, or a mesh shader with or without its task shader), then the fragment's.
    struct Stage { VkShaderStageFlagBits stage; const char* file; const char* entry; const VkSpecializationInfo* spec; };
    const Stage wanted[] = {
        {VK_SHADER_STAGE_VERTEX_BIT, d.mesh ? nullptr : d.vertex, d.vertexEntry, d.vertexSpecialization},
        {VK_SHADER_STAGE_TASK_BIT_EXT, d.mesh ? d.task : nullptr, d.taskEntry, nullptr},
        {VK_SHADER_STAGE_MESH_BIT_EXT, d.mesh, d.meshEntry, nullptr},
        {VK_SHADER_STAGE_FRAGMENT_BIT, d.fragment, d.fragmentEntry, d.fragmentSpecialization},
    };
    VkPipelineShaderStageCreateInfo stages[4]{};
    uint32_t stageCount = 0;
    bool loaded = d.vertex || d.mesh;
    for (const Stage& w : wanted) {
        if (!w.file) continue;
        const VkShaderModule module = rtc::loadShaderModule(device, w.file);
        if (module == VK_NULL_HANDLE) {
            loaded = false;
            break;
        }
        stages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stageCount].stage = w.stage;
        stages[stageCount].module = module;
        stages[stageCount].pName = w.entry;
        stages[stageCount].pSpecializationInfo = w.spec;
        ++stageCount;
    }
    const auto destroyModules = [&] {
        for (uint32_t i = 0; i < stageCount; ++i) vkDestroyShaderModule(device, stages[i].module, nullptr);
    };
    if (!loaded) {
        destroyModules();
        return VK_NULL_HANDLE;
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
    info.pVertexInputState = d.mesh ? nullptr : &vertexInput;
    info.pInputAssemblyState = d.mesh ? nullptr : &assembly;
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
