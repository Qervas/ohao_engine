#pragma once
#include <array>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace ohao {

class OhaoVkDevice;
class OhaoVkRenderPass;
class OhaoVkSwapChain;
class OhaoVkShaderModule;

struct PipelineConfigInfo {
    VkViewport viewport;
    VkRect2D scissor;
    VkPipelineViewportStateCreateInfo viewportInfo;
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo;
    VkPipelineRasterizationStateCreateInfo rasterizationInfo;
    VkPipelineMultisampleStateCreateInfo multisampleInfo;
    VkPipelineColorBlendAttachmentState colorBlendAttachment;
    VkPipelineColorBlendStateCreateInfo colorBlendInfo;
    VkPipelineDepthStencilStateCreateInfo depthStencilInfo;
    VkPipelineDynamicStateCreateInfo dynamicStateInfo;
    std::vector<VkDynamicState> dynamicStateEnables;
    PipelineConfigInfo() {
        // Zero initialize all structs
        viewport = {};
        scissor = {};
        viewportInfo = {};
        inputAssemblyInfo = {};
        rasterizationInfo = {};
        multisampleInfo = {};
        colorBlendAttachment = {};
        colorBlendInfo = {};
        depthStencilInfo = {};
        dynamicStateInfo = {};

        // Set sTypes
        viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        rasterizationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        multisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        colorBlendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        depthStencilInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;

        // Initialize all pNext pointers to nullptr
        viewportInfo.pNext = nullptr;
        inputAssemblyInfo.pNext = nullptr;
        rasterizationInfo.pNext = nullptr;
        multisampleInfo.pNext = nullptr;
        colorBlendInfo.pNext = nullptr;
        depthStencilInfo.pNext = nullptr;
        dynamicStateInfo.pNext = nullptr;

        // Initialize all flags to 0
        viewportInfo.flags = 0;
        inputAssemblyInfo.flags = 0;
        rasterizationInfo.flags = 0;
        multisampleInfo.flags = 0;
        colorBlendInfo.flags = 0;
        depthStencilInfo.flags = 0;
        dynamicStateInfo.flags = 0;
        dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateInfo.pNext = nullptr;
        dynamicStateInfo.dynamicStateCount = 0;
        dynamicStateInfo.pDynamicStates = nullptr;
    }

    PipelineConfigInfo(const PipelineConfigInfo& other){
        viewport = other.viewport;
        scissor = other.scissor;
        viewportInfo = other.viewportInfo;
        inputAssemblyInfo = other.inputAssemblyInfo;
        rasterizationInfo = other.rasterizationInfo;
        multisampleInfo = other.multisampleInfo;
        colorBlendAttachment = other.colorBlendAttachment;
        colorBlendInfo = other.colorBlendInfo;
        depthStencilInfo = other.depthStencilInfo;
        dynamicStateEnables = other.dynamicStateEnables;
        dynamicStateInfo = other.dynamicStateInfo;
        dynamicStateInfo.pDynamicStates = dynamicStateEnables.data();
    }
};

class OhaoVkPipeline {
public:
    enum class RenderMode {
        SOLID,
        WIREFRAME,
        GIZMO,
        PUSH_CONSTANT_MODEL,
        SHADOW
    };
    struct SelectionPushConstants {
        glm::vec4 highlightColor;
        float scaleOffset;
    };
    
    struct ModelPushConstants {
        glm::mat4 model;
        glm::vec3 baseColor;
        float metallic;
        float roughness;
        float ao;
        glm::vec2 padding; // Ensure 16-byte alignment
    };
    
    OhaoVkPipeline() = default;
    ~OhaoVkPipeline();

    bool initialize(
        OhaoVkDevice* device,
        OhaoVkRenderPass* renderPass,
        OhaoVkShaderModule* shaderModule,
        VkExtent2D swapChainExtent,
        VkDescriptorSetLayout descriptorSetLayout,
        RenderMode mode,
        const PipelineConfigInfo* configInfo = nullptr,
        VkPipelineLayout layout = VK_NULL_HANDLE);

    // Overload for raw VkRenderPass (used for shadow mapping)
    bool initialize(
        OhaoVkDevice* device,
        VkRenderPass rawRenderPass,
        OhaoVkShaderModule* shaderModule,
        VkExtent2D swapChainExtent,
        VkDescriptorSetLayout descriptorSetLayout,
        RenderMode mode,
        const PipelineConfigInfo* configInfo = nullptr,
        VkPipelineLayout layout = VK_NULL_HANDLE);

    void cleanup();

    void bind(VkCommandBuffer commandBuffer);

    VkPipeline getPipeline() const { return graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }
    RenderMode getRenderMode() const { return renderMode; }


    void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo, VkExtent2D extent);
    static bool createSelectionPipelineLayout(
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        VkPipelineLayout& pipelineLayout);

private:
    bool createPipeline(RenderMode mode, const PipelineConfigInfo* configInfo = nullptr);
    bool createPipelineLayoutWithPushConstants(VkDescriptorSetLayout descriptorSetLayout);
    bool createModelPushConstantPipelineLayout(VkDescriptorSetLayout descriptorSetLayout);
    bool createDefaultPipelineLayout(VkDescriptorSetLayout descriptorSetLayout);

    OhaoVkDevice* device{nullptr};
    OhaoVkRenderPass* renderPass{nullptr};
    VkRenderPass rawRenderPass{VK_NULL_HANDLE};  // For shadow mapping (when not using wrapper)
    OhaoVkShaderModule* shaderModule{nullptr};
    VkExtent2D extent{};

    VkPipeline graphicsPipeline{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    RenderMode renderMode{RenderMode::SOLID};
};

} // namespace ohao
