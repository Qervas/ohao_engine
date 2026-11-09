#include "ohao/render_host.hpp"
#include "engine/install.hpp"

#include "world_render/instantiate.hpp"
#include "world_render/render_extract.hpp"
#include "world_render/vfx_draw.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "runtime/frame_capture.hpp"

namespace ohao::host {

namespace {

reflect::Diagnostic renderError(std::string message) {
    reflect::Diagnostic d;
    d.code = "E_RENDER";
    d.message = std::move(message);
    return d;
}

} // namespace

RenderHost::RenderHost(ecs::AssetResolver& assets, api::HostKind kind) : m_assets(assets), m_kind(kind) {}

// The World's systems hold references into the renderer's RenderScene; the World is
// destroyed first (see the header), so the renderer can go now.
RenderHost::~RenderHost() = default;

std::optional<api::RenderStats> RenderHost::renderStats(int frames) {
    if (!m_renderer) return std::nullopt;
    return runtime::renderStats(*m_renderer, frames);
}

std::optional<api::RenderSettings> RenderHost::renderSettings() {
    if (m_kind == api::HostKind::Cli) return std::nullopt;
    if (m_renderer) return m_renderer->renderSettings();
    return m_settings;
}

void RenderHost::setRenderSettings(const api::RenderSettings& settings) {
    if (m_kind == api::HostKind::Cli) return;
    m_settings = settings;
    m_settingsSet = true;
    if (m_renderer) m_renderer->setRenderSettings(settings);
}

std::optional<api::Host::GpuInfo> RenderHost::gpuInfo() {
    if (!m_renderer) return std::nullopt;
    return runtime::gpuInfo(*m_renderer);
}

api::RenderOutcome RenderHost::capture(const api::RenderRequest& r) {
    api::RenderOutcome out;
    if (!m_world) {
        out.errors.push_back(renderError("the render host has no world"));
        return out;
    }
    const uint32_t width = r.width.value_or(1920), height = r.height.value_or(1080);
    if (!m_renderer) {
        if (m_failed) {
            out.errors.push_back(renderError("the renderer could not be started (see the log)"));
            return out;
        }
        auto renderer = std::make_unique<VulkanRenderer>(width, height);
        if (!renderer->initialize()) {
            m_failed = true;
            out.errors.push_back(renderError("no usable GPU: the renderer could not be initialized"));
            return out;
        }
        m_renderer = std::move(renderer);
        if (auto* deferred = m_renderer->getDeferredRenderer())
            if (auto* pp = deferred->getPostProcessing()) {
                m_defaultEv100 = pp->ev100();
                m_defaultCurve = pp->displayCurve();
            }
        engine::installRendering(*m_world, m_renderer->renderScene(), m_renderer->getTextureManager(), m_assets);
        if (m_settingsSet) m_renderer->setRenderSettings(m_settings);
    } else {
        m_renderer->resize(width, height);   // a no-op at the same size
    }
    runtime::CaptureSettings settings;
    settings.reset = m_captures > 0;   // the first capture renders exactly as a fresh renderer does (the goldens)
    settings.defaultEv100 = m_defaultEv100;
    settings.defaultCurve = m_defaultCurve;
    settings.keepPixels = &m_lastPixels;
    m_lastPixels.clear();
    ++m_captures;
    return runtime::captureFrame(*m_renderer, *m_world, m_assets, r, settings);
}

} // namespace ohao::host
