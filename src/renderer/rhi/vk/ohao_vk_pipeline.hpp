#pragma once
#include <array>
#include <vulkan/vulkan.h>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace ohao {

class OhaoVkDevice;
class OhaoVkRenderPass;
class OhaoVkSwapChain;
class OhaoVkShaderModule;

struct PipelineConfigInfo {
    VkPipelineViewportStateCreateInfo viewportInfo;
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo;
    VkPipelineRasterizationStateCreateInfo rasterizationInfo;
    VkPipelineMultisampleStateCreateInfo multisampleInfo;
    VkPipelineColorBlendAttachmentState colorBlendAttachment;
    VkPipelineColorBlendStateCreateInfo colorBlendInfo;
    VkPipelineDepthStencilStateCreateInfo depthStencilInfo;
    std::vector<VkDynamicState> dynamicStateEnables;
    VkPipelineDynamicStateCreateInfo dynamicStateInfo;
    VkPipelineLayout pipelineLayout{nullptr};
    VkRenderPass renderPass{nullptr};
    uint32_t subpass{0};
};

class OhaoVkPipeline {
public:
    enum class RenderMode {
        SOLID,
        WIREFRAME,
        GIZMO
    };
    OhaoVkPipeline() = default;
    ~OhaoVkPipeline();

    bool initialize(
        OhaoVkDevice* device,
        OhaoVkRenderPass* renderPass,
        OhaoVkShaderModule* shaderModule,
        VkExtent2D swapChainExtent,
        VkDescriptorSetLayout descriptorSetLayout,
        RenderMode mode);

    void cleanup();

    void bind(VkCommandBuffer commandBuffer);

    VkPipeline getPipeline() const { return graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

    static void defaultPipelineConfigInfo(PipelineConfigInfo& configInfo);

private:
    bool createPipelineLayout(VkDescriptorSetLayout descriptorSetLayout);
    bool createPipeline(RenderMode mode);

    OhaoVkDevice* device{nullptr};
    OhaoVkRenderPass* renderPass{nullptr};
    OhaoVkShaderModule* shaderModule{nullptr};
    VkExtent2D extent{};

    VkPipeline graphicsPipeline{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    RenderMode renderMode{RenderMode::SOLID};
};

} // namespace ohao
