#include "runtime/live_host.hpp"

#include "core/log.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_view.hpp"
#include "level/level_system.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "runtime/frame_capture.hpp"
#include "runtime/scene_view_apply.hpp"

#include <algorithm>
#include <utility>

namespace ohao::runtime {

namespace {

using Clock = std::chrono::steady_clock;

reflect::Diagnostic error(std::string code, std::string message) {
    reflect::Diagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

// render.capture's mode names (the session checked them).
RenderMode modeOf(std::string_view name) {
    return name == "rt_realtime" ? RenderMode::RTRealtime : name == "rt_offline" ? RenderMode::RTOffline : RenderMode::Deferred;
}
std::string_view modeName(RenderMode m) {
    return m == RenderMode::RTRealtime ? "rt_realtime" : m == RenderMode::RTOffline ? "rt_offline" : "deferred";
}

// The window's live post settings, kept across a lockstep capture (which sets its own, and
// its own quality). A realtime capture is the next live frame, with the live settings.
struct PostState {
    bool bloom{false}, taa{false}, ssao{false};
    float exposure{1.0f};
};

} // namespace

bool LiveHost::initialize(const LiveConfig& cfg, const reflect::TypeRegistry& types) {
    m_cfg = cfg;
    m_time = Timeline(cfg.timeMode, cfg.clock);
    if (!m_window.create(cfg.window)) return false;
    const VkExtent2D ext = m_window.framebufferExtent();
    m_renderer = std::make_unique<VulkanRenderer>(ext.width, ext.height);
    if (!m_renderer->initialize(m_window.presentTarget(cfg.vsync))) {
        OHAO_LOG(Error, runtime, "live host: renderer initialization failed");
        return false;
    }
    m_renderer->setRenderMode(RenderMode::Deferred);
    if (auto* deferred = m_renderer->getDeferredRenderer())
        if (auto* pp = deferred->getPostProcessing()) m_defaultExposure = pp->getExposure();
    if (cfg.mode != RenderMode::Deferred) {
        m_renderer->setRenderMode(cfg.mode);
        if (m_renderer->getRenderMode() != cfg.mode)
            OHAO_LOG(Warn, runtime, "live host: this GPU has no ray tracing; the window presents deferred frames");
    }
    m_liveMode = m_renderer->getRenderMode();
    m_assets = std::make_unique<ecs::FileAssetResolver>(cfg.root);
    m_world = std::make_unique<ecs::World>();
    ecs::installRenderExtract(*m_world, m_renderer->renderScene());
    ecs::installInstantiate(*m_world, m_renderer->renderScene(), m_renderer->getTextureManager(), *m_assets);
    level::installLevels(*m_world, *m_assets, types);   // before instantiate: Level components expand into entities
    m_session = std::make_unique<api::Session>(*m_world, types);
    m_session->setAssetResolver(m_assets.get());
    m_session->setTimeline(m_time);
    m_session->setHost(this);
    return true;
}

void LiveHost::renderFrame() {
    const bool changed = m_needRender || m_session->changeCount() != m_renderedChange;
    m_world->sync();   // instantiate, transforms, the render extract: never a simulation step
    for (const auto& w : applySceneView(*m_renderer, ecs::resolveSceneView(*m_world), *m_assets).warnings)
        OHAO_LOG(Warn, runtime, "live host: {}", w);
    // The path tracer restarts its accumulation when the view may have moved (it notices
    // scene uploads itself); a still world keeps accumulating, one sample pass a frame.
    if (m_renderer->getRenderMode() != RenderMode::Deferred && changed) m_renderer->notifyCameraChanged();
    m_liveSamples = changed ? 1 : m_liveSamples + 1;
    m_renderer->render();
    ++m_rendered;
    m_renderedChange = m_session->changeCount();
    m_needRender = false;
}

RunReport LiveHost::run(const GameHooks& hooks, const RequestPump* pump) {
    if (hooks.start) hooks.start(*this);

    auto last = Clock::now();
    uint64_t frame = 0;
    while (!m_quit && !m_window.shouldClose()) {
        if (m_cfg.frameLimit > 0 && frame >= static_cast<uint64_t>(m_cfg.frameLimit)) break;
        if (static_cast<int64_t>(frame) == m_cfg.resizeAtFrame)
            m_window.setSize(m_cfg.resizeTo.width, m_cfg.resizeTo.height);

        m_window.poll(m_events);   // appended: kept until a step consumes them
        if (m_window.consumeResized()) {
            m_renderer->setPresentExtent(m_window.framebufferExtent());
            m_needRender = true;
        }

        // Requests run at this point of the frame, in arrival order. While nothing steps,
        // the loop waits here for one (at most 16 ms, so the window stays responsive).
        if (pump && pump->poll && !pump->poll(std::chrono::milliseconds(m_time.running() ? 0 : 16))) m_quit = true;

        const auto now = Clock::now();
        const Tick tick = m_time.advance(std::chrono::duration_cast<Nanos>(now - last));
        last = now;
        if (!m_time.running()) {   // lockstep or paused: the window's input reaches no step
            m_events.clear();
            m_releaseOnResume = true;
        }
        const float fixedDt = m_time.fixedDt();
        for (int i = 0; i < tick.steps; ++i) {
            if (std::exchange(m_releaseOnResume, false))
                m_events.insert(m_events.begin(), InputEvent{InputEvent::Type::FocusLost});
            m_session->step(m_events, fixedDt);
            m_events.clear();
            if (hooks.fixedUpdate) hooks.fixedUpdate(*this, fixedDt);
        }
        const float frameDt = std::chrono::duration<float>(tick.frameDt).count();
        if (hooks.update) hooks.update(*this, frameDt, tick.alpha);

        const VkExtent2D ext = m_window.framebufferExtent();
        // rt_offline keeps presenting its accumulation until spp samples, while nothing steps.
        const bool converging = m_liveMode == RenderMode::RTOffline && m_liveSamples < m_cfg.spp;
        const bool due =
            m_time.running() || m_needRender || m_session->changeCount() != m_renderedChange || converging;
        if (ext.width != 0 && ext.height != 0 && due) {   // minimized: simulate, don't render
            renderFrame();
            // Whole frame time (input + simulation + render submit), so fps is the real
            // frame rate; GPU time comes from the deferred timestamp queries.
            m_stats.record(std::chrono::duration<double, std::milli>(tick.frameDt).count(), m_renderer->gpuFrameMs());
            m_time.setFps(static_cast<float>(m_stats.fps()));
        }
        if (pump && pump->flush) pump->flush();   // replies after the frame they caused
        ++frame;
    }
    if (pump && pump->flush) pump->flush();

    RunReport r;
    r.frames = frame;
    r.rendered = m_rendered;
    r.readbackCopies = unrequestedReadbacks();
    r.requestedReadbacks = m_renderer->requestedReadbackCount();
    r.validationMessages = m_renderer->validationMessageCount();
    r.validationEnabled = m_renderer->validationEnabled();
    r.avgFrameMs = m_stats.avgCpuMs();
    r.avgGpuMs = m_stats.avgGpuMs();
    r.fps = m_stats.fps();
    return r;
}

uint64_t LiveHost::unrequestedReadbacks() const noexcept {
    const uint64_t copies = m_renderer ? m_renderer->requestedReadbackCount() : 0;
    return copies > m_readbackRequests ? copies - m_readbackRequests : 0;
}

std::optional<api::RenderStats> LiveHost::renderStats(int frames) {
    if (!m_renderer) return std::nullopt;
    return runtime::renderStats(*m_renderer, frames);
}

std::optional<api::RenderSettings> LiveHost::renderSettings() {
    if (!m_renderer) return std::nullopt;
    return readRenderSettings(*m_renderer);
}

void LiveHost::setRenderSettings(const api::RenderSettings& settings) {
    if (!m_renderer) return;
    applyRenderSettings(*m_renderer, settings);
    m_needRender = true;
}

std::optional<api::Host::GpuInfo> LiveHost::gpuInfo() {
    if (!m_renderer) return std::nullopt;
    return runtime::gpuInfo(*m_renderer);
}

api::RenderOutcome LiveHost::capture(const api::RenderRequest& r) {
    api::RenderOutcome out;
    const VkExtent2D ext = m_window.framebufferExtent();
    if (ext.width == 0 || ext.height == 0) {
        out.errors.push_back(error("E_STATE", "the window is minimized: there is no frame to capture"));
        return out;
    }
    const RenderMode mode = modeOf(r.mode);
    if (mode != RenderMode::Deferred && !m_renderer->isRTSupported()) {
        out.errors.push_back(error("E_COMMAND", "this GPU has no ray tracing: a window host captures deferred frames only"));
        return out;
    }
    if ((r.width && *r.width != ext.width) || (r.height && *r.height != ext.height)) {
        out.errors.push_back(error("E_COMMAND", "a window host captures at its own size, " + std::to_string(ext.width) +
                                                    "x" + std::to_string(ext.height) + "; leave width and height out"));
        return out;
    }
    if (m_time.running()) {
        // Realtime: the next frame the window presents, as is (no reset, the live settings),
        // so in the window's live mode.
        if (mode != m_liveMode) {
            out.errors.push_back(error("E_COMMAND", "a realtime window captures the frames it presents, " +
                                                        std::string(modeName(m_liveMode)) + "; capture " + r.mode +
                                                        " in lockstep (time.set), or start the window with --mode " +
                                                        r.mode));
            return out;
        }
        if (mode != RenderMode::Deferred && r.denoise != "none") {
            out.errors.push_back(error("E_COMMAND", "a realtime window's frame is captured as presented, undenoised; "
                                                    "capture in lockstep (time.set) for a denoised frame"));
            return out;
        }
        const auto start = Clock::now();
        m_world->sync();
        for (const auto& w : applySceneView(*m_renderer, ecs::resolveSceneView(*m_world), *m_assets).warnings)
            OHAO_LOG(Warn, runtime, "live host: {}", w);
        // The renderer's size before the frame: a pending resize applies in render(), and a
        // region checked against the old size could leave the new frame.
        out.width = ext.width;
        out.height = ext.height;
        if (auto e = checkRegion(r, out.width, out.height); !e.empty()) {
            out.errors = std::move(e);
            return out;
        }
        ++m_readbackRequests;
        const uint64_t framesBefore = m_renderer->framesSubmitted();
        const bool previewOnly = requestCaptureReadback(*m_renderer, r, false);
        m_renderer->render();
        ++m_rendered;
        if (m_renderer->getWidth() != out.width || m_renderer->getHeight() != out.height) {
            out.errors.push_back(error("E_STATE", "the window did not present a frame (minimized or resizing)"));
            return out;
        }
        readCaptureOutputs(*m_renderer, r, previewOnly, framesBefore, out);
        if (!out.errors.empty()) {
            // No frame reached the staging buffer: the swapchain was out of date or minimized.
            for (auto& d : out.errors)
                if (d.code == "E_RENDER") {
                    d.code = "E_STATE";
                    d.message = "the window did not present a frame (minimized or resizing)";
                }
            return out;
        }
        out.frames = 1;
        out.deterministic = false;
        out.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    } else {
        // Lockstep or paused: the world as it is, rendered as a headless host renders it.
        // A pending resize (the swapchain rebuilds at the next frame) is applied first, so
        // the capture is at the window's size.
        if (m_renderer->getWidth() != ext.width || m_renderer->getHeight() != ext.height) {
            m_world->sync();
            m_renderer->render();
            ++m_rendered;
        }
        PostState live;
        auto* pp = m_renderer->getDeferredRenderer() ? m_renderer->getDeferredRenderer()->getPostProcessing() : nullptr;
        if (pp) live = {pp->getBloomEnabled(), pp->getTAAEnabled(), pp->getSSAOEnabled(), pp->getExposure()};
        const RenderQuality liveQuality = m_renderer->renderQuality();
        const DenoiseMode liveDenoise = m_renderer->getDenoiseMode();
        CaptureSettings settings;
        settings.reset = true;
        settings.defaultExposure = m_defaultExposure;
        // Every frame is presented: a path-traced capture's sample passes converge on screen.
        out = captureFrame(*m_renderer, *m_world, *m_assets, r, settings);
        if (!out.cancelled) ++m_readbackRequests;   // its last frame was requested (at most one)
        m_rendered += static_cast<uint64_t>(std::max(0, out.frames));
        if (pp) {
            pp->setBloomEnabled(live.bloom);
            pp->setTAAEnabled(live.taa);
            pp->setSSAOEnabled(live.ssao);
            pp->setExposure(live.exposure);
        }
        m_renderer->setRenderQuality(liveQuality);
        if (m_renderer->getDenoiseMode() != liveDenoise) m_renderer->setDenoiseMode(liveDenoise);
        if (m_renderer->getRenderMode() != m_liveMode) {
            // Back to what the window presents, with no history from before the capture.
            m_renderer->setRenderMode(m_liveMode);
            m_renderer->resetAccumulation();
        }
        // The capture's last frame was presented and shows the current world: no frame
        // follows until something changes (one in flight would delay the next capture's
        // reset). A capture in the live mode counts as that many samples of it.
        m_renderedChange = m_session->changeCount();
        m_liveSamples = mode == m_liveMode ? std::max(0, out.frames) : m_cfg.spp;
        if (out.cancelled) m_needRender = true;   // the window redraws what it shows live
    }
    if (m_renderer->validationEnabled()) out.validationMessages = m_renderer->validationMessageCount();
    return out;
}

uint64_t LiveHost::shutdown() {
    if (m_shutDown) return 0;
    m_shutDown = true;
    uint64_t validation = 0;
    const bool validated = m_renderer && m_renderer->validationEnabled();
    const uint64_t unrequested = unrequestedReadbacks();
    const uint64_t requested = m_renderer ? m_renderer->requestedReadbackCount() : 0;
    m_session.reset();
    m_world.reset();   // releases its RenderScene slots while the RenderScene exists
    if (m_renderer) {
        m_renderer->shutdown();   // device and surface go before the window
        validation = m_renderer->validationMessageCount();
        m_renderer.reset();
    }
    m_window.destroy();
    OHAO_LOG(Info, runtime,
             "live host: {} frames rendered; readbacks {} unrequested, {} requested; validation {} including "
             "teardown",
             m_rendered, unrequested, requested, validated ? std::to_string(validation) : std::string("off"));
    return validation;
}

} // namespace ohao::runtime
