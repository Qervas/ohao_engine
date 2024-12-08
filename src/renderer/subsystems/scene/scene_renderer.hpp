#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include "rhi/vk/ohao_vk_texture_handle.hpp"

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
    void render();
    void endFrame();

    OhaoVkTextureHandle getViewportTexture() const;
    void resize(uint32_t width, uint32_t height);
    ViewportSize getViewportSize() const;

private:
    VulkanContext* context{nullptr};
    std::unique_ptr<SceneRenderTarget> renderTarget;

    bool createRenderResources(uint32_t width, uint32_t height);
};

} // namespace ohao
