#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <renderer/rhi/vk/ohao_vk_uniform_buffer.hpp>
#include <renderer/gizmo/axis_gizmo.hpp>
#include "renderer/rhi/vk/ohao_vk_texture_handle.hpp"
#include "renderer/rhi/vk/ohao_vk_pipeline.hpp"
#include "engine/actor/actor.hpp"
#include "utils/common_types.hpp"

namespace ohao {

class VulkanContext;
class SceneRenderTarget;
struct ViewportSize {
    uint32_t width;
    uint32_t height;
};
class SceneRenderer {
public:
    SceneRenderer() = default;
    ~SceneRenderer();

    bool initialize(VulkanContext* context);
    void cleanup();

    bool initializeRenderTarget(uint32_t width, uint32_t height);

    void beginFrame();
    void render(OhaoVkUniformBuffer* uniformBuffer, uint32_t currentFrame);
    void endFrame();

    OhaoVkTextureHandle getViewportTexture() const;
    void resize(uint32_t width, uint32_t height);
    ViewportSize getViewportSize() const;
    SceneRenderTarget* getRenderTarget() const { return renderTarget.get(); }
    bool hasValidRenderTarget() const;
    void setPipelines(OhaoVkPipeline* mainPipeline, OhaoVkPipeline* gizmoPipeline) {
        pipeline = mainPipeline;
        this->gizmoPipeline = gizmoPipeline;
    }
    
    void setPipelinesWithWireframe(OhaoVkPipeline* solidPipeline, OhaoVkPipeline* wireframePipeline, OhaoVkPipeline* gizmoPipeline) {
        this->solidPipeline = solidPipeline;
        this->wireframePipeline = wireframePipeline;
        this->gizmoPipeline = gizmoPipeline;
        pipeline = solidPipeline;  // Default to solid
    }
    
    void setWireframeMode(bool wireframe) {
        if (solidPipeline && wireframePipeline) {
            pipeline = wireframe ? wireframePipeline : solidPipeline;
            isWireframeMode = wireframe;
        }
    }
    
    AxisGizmo* getAxisGizmo() const { return axisGizmo.get(); }
    
    static void defaultSelectionPipelineConfig(PipelineConfigInfo& configInfo, VkExtent2D extent);

private:
    VulkanContext* context{nullptr};
    std::unique_ptr<SceneRenderTarget> renderTarget;
    OhaoVkPipeline* pipeline{nullptr};
    OhaoVkPipeline* solidPipeline{nullptr};
    OhaoVkPipeline* wireframePipeline{nullptr};
    std::unique_ptr<AxisGizmo> axisGizmo;
    OhaoVkPipeline* gizmoPipeline{nullptr};
    std::unique_ptr<OhaoVkPipeline> selectionPipeline;
    VkPipelineLayout selectionPipelineLayout{VK_NULL_HANDLE};
    VkCommandBuffer currentCommandBuffer{VK_NULL_HANDLE};
    bool isPipelineLayoutValid{false};  // Track if pipeline layout was successfully created
    bool isWireframeMode{false};  // Track wireframe state

    struct SelectionPushConstants {
        glm::vec4 highlightColor;
        float scaleOffset;
    };

    bool createRenderResources(uint32_t width, uint32_t height);
    bool initializeSelectionPipeline();
    void drawSelectionHighlight(VkCommandBuffer cmd, Actor* actor, const MeshBufferInfo& bufferInfo);
    void renderAxisGizmo(VkCommandBuffer cmd, OhaoVkUniformBuffer* uniformBuffer, uint32_t currentFrame);
};

} // namespace ohao
