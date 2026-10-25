#pragma once
// The headless host behind render.capture (ohao run, ohao serve without a window, the GPU
// tests). The renderer starts on the first capture (a session that never renders creates
// no Vulkan device), and the render extract and instantiate are installed into the
// session's world then; they pick up the entities it already has. A capture is
// runtime::captureFrame, the procedure the window host runs too: the world's Camera and
// Environment, `frames` frames from a reset, the last one read back.
//
// A capture renders as cornell_box and model_viewer do (world.sync() then render(), per
// frame), so `ohao run` reproduces their golden images from the scene files. It never runs
// the simulate phase: capturing does not change the world.
//
// Declare the RenderHost before the World: the World's systems release their RenderScene
// slots when it is destroyed, so the renderer must outlive it.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ohao { class VulkanRenderer; }

namespace ohao::host {

class RenderHost final : public api::Host {
public:
    explicit RenderHost(ecs::AssetResolver& assets, api::HostKind kind = api::HostKind::Cli);
    ~RenderHost() override;
    RenderHost(const RenderHost&) = delete;
    RenderHost& operator=(const RenderHost&) = delete;

    // The world captures render (once; before the first capture).
    void bind(ecs::World& world) noexcept { m_world = &world; }

    [[nodiscard]] api::HostKind kind() const noexcept override { return m_kind; }
    // render.capture (api::Session::setHost). The default size is 1920x1080.
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest& request) override;
    // render.stats over the frames captures rendered; nullopt before the first capture.
    [[nodiscard]] std::optional<api::RenderStats> renderStats(int frames) override;
    // render.settings (headless only; the CLI host has none: E_STATE). Kept until the renderer
    // starts, then applied to it; read back from it once it runs.
    [[nodiscard]] std::optional<api::RenderSettings> renderSettings() override;
    void setRenderSettings(const api::RenderSettings& settings) override;
    // engine.info's gpu, once the first capture started the renderer.
    [[nodiscard]] std::optional<GpuInfo> gpuInfo() override;

    // The renderer, once started; null before the first capture or if it failed.
    [[nodiscard]] VulkanRenderer* renderer() noexcept { return m_renderer.get(); }
    // The last capture's whole frame, RGBA8 (before region and preview), as read back.
    [[nodiscard]] const std::vector<uint8_t>& lastPixels() const noexcept { return m_lastPixels; }

private:
    std::vector<uint8_t> m_lastPixels;
    ecs::AssetResolver& m_assets;
    api::HostKind m_kind;
    ecs::World* m_world{nullptr};
    std::unique_ptr<VulkanRenderer> m_renderer;
    bool m_failed{false};
    int m_captures{0};
    float m_defaultExposure{1.0f};   // read from the renderer at startup; a capture without one uses it
    api::RenderSettings m_settings;  // before the renderer starts
    bool m_settingsSet{false};       // render.settings changed them: applied when the renderer starts
};

} // namespace ohao::host
