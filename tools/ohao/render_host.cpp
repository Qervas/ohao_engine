#include "ohao/render_host.hpp"
#include "engine/install.hpp"

#include "ecs/module.hpp"
#include "render/render_thread.hpp"
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

RenderHost::RenderHost(api::HostKind kind) : m_kind(kind) {}

// The World's systems hold references into the renderer's RenderScene; the World is
// destroyed first (see the header), so the renderer can go now.
RenderHost::~RenderHost() = default;

std::optional<api::RenderStats> RenderHost::renderStats(int frames) {
    if (!m_renderer) return std::nullopt;
    return m_renderThread->run([&] { return runtime::renderStats(*m_renderer, frames); });
}

std::optional<api::RenderSettings> RenderHost::renderSettings() {
    if (m_kind == api::HostKind::Cli) return std::nullopt;
    if (m_renderer) return m_renderThread->run([&] { return m_renderer->renderSettings(); });
    return m_settings;
}

void RenderHost::setRenderSettings(const api::RenderSettings& settings) {
    if (m_kind == api::HostKind::Cli) return;
    m_settings = settings;
    m_settingsSet = true;
    if (m_renderer) m_renderThread->run([&] { m_renderer->setRenderSettings(settings); });
}

std::optional<api::Host::GpuInfo> RenderHost::gpuInfo() {
    if (!m_renderer) return std::nullopt;
    return m_renderThread->run([&] { return runtime::gpuInfo(*m_renderer); });
}

api::RenderOutcome RenderHost::capture(const api::RenderRequest& r) {
    api::RenderOutcome out;
    if (!m_world) {
        out.errors.push_back(renderError("the render host has no world"));
        return out;
    }
    const uint32_t width = r.width.value_or(1920), height = r.height.value_or(1080);
    if (!m_renderer && !ecs::worldServices(*m_world)) {
        out.errors.push_back(renderError("the render host's world has no engine module installed (engine::install)"));
        return out;
    }
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
        m_renderThread = std::make_unique<render::RenderThread>(*m_renderer);
        if (auto* deferred = m_renderer->getDeferredRenderer())
            if (auto* pp = deferred->getPostProcessing()) {
                m_defaultEv100 = pp->ev100();
                m_defaultCurve = pp->displayCurve();
            }
        // world_render waited for the renderer (and what it depends on, if the world lacks it).
        ecs::Services services = *ecs::worldServices(*m_world);
        services.put(m_renderer->renderScene());
        if (BindlessTextureManager* textures = m_renderer->getTextureManager()) services.put(*textures);
        services.put(*m_renderThread);   // its loads go through the render thread
        engine::install(*m_world, services, {"world_render"});
        if (m_settingsSet) m_renderer->setRenderSettings(m_settings);
        if (m_threaded) m_renderThread->start();
    } else {
        m_renderThread->run([&] { m_renderer->resize(width, height); });   // a no-op at the same size
    }
    return m_renderThread->run([&] { return captureHeld(r); });
}

api::RenderOutcome RenderHost::captureHeld(const api::RenderRequest& r) {
    runtime::CaptureSettings settings;
    settings.reset = m_captures > 0;   // the first capture renders exactly as a fresh renderer does (the goldens)
    settings.defaultEv100 = m_defaultEv100;
    settings.defaultCurve = m_defaultCurve;
    settings.keepPixels = &m_lastPixels;
    m_lastPixels.clear();
    ++m_captures;
    return runtime::captureFrame(*m_renderer, *m_world, r, settings);
}

} // namespace ohao::host
