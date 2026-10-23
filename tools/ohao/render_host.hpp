#pragma once
// The GPU side of the ohao CLI: render.capture's handler. The renderer starts on the
// first capture (a session that never renders creates no Vulkan device), and the render
// extract and instantiate are installed into the session's world then; they pick up the
// entities it already has. Each capture applies the world's Camera and Environment
// (scene_host.hpp), renders the frames and writes a PNG.
//
// A capture renders exactly as cornell_box and model_viewer do (world.update(0) then
// render(), per frame; the same frame counts and settings), so `ohao run` reproduces
// their golden images from the scene files.
//
// Declare the RenderHost before the World: the World's systems release their RenderScene
// slots when it is destroyed, so the renderer must outlive it.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"

#include <memory>

namespace ohao { class VulkanRenderer; }

namespace ohao::host {

class RenderHost {
public:
    explicit RenderHost(ecs::AssetResolver& assets);
    ~RenderHost();
    RenderHost(const RenderHost&) = delete;
    RenderHost& operator=(const RenderHost&) = delete;

    // The world captures render (once; before the first capture).
    void bind(ecs::World& world) noexcept { m_world = &world; }

    // render.capture (api::Session::setRenderHandler).
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest& request);

    // The renderer, once started; null before the first capture or if it failed.
    [[nodiscard]] VulkanRenderer* renderer() noexcept { return m_renderer.get(); }

private:
    ecs::AssetResolver& m_assets;
    ecs::World* m_world{nullptr};
    std::unique_ptr<VulkanRenderer> m_renderer;
    bool m_failed{false};
    int m_captures{0};
    float m_defaultExposure{1.0f};   // read from the renderer at startup; a capture without one uses it
};

} // namespace ohao::host
