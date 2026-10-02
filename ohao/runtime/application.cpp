#include "runtime/application.hpp"
#include <chrono>
#include <iostream>

namespace ohao::runtime {

bool Application::initialize(const AppConfig& cfg) {
    m_cfg = cfg;
    m_clock = FixedStepClock(cfg.clock);
    if (!m_window.create(cfg.window)) return false;
    const VkExtent2D ext = m_window.framebufferExtent();
    m_renderer = std::make_unique<VulkanRenderer>(ext.width, ext.height);
    if (!m_renderer->initialize(m_window.presentTarget(cfg.vsync))) {
        std::cerr << "[App] renderer initialization failed" << std::endl;
        return false;
    }
    m_renderer->setRenderMode(RenderMode::Deferred);
    return true;
}

RunReport Application::run(const GameHooks& hooks) {
    using Clock = std::chrono::steady_clock;
    if (hooks.start) hooks.start(*this);

    auto last = Clock::now();
    int frame = 0;
    while (!m_quit && !m_window.shouldClose()) {
        if (m_cfg.frameLimit > 0 && frame >= m_cfg.frameLimit) break;
        if (frame == m_cfg.resizeAtFrame) m_window.setSize(m_cfg.resizeTo.width, m_cfg.resizeTo.height);

        m_events.clear();
        m_window.poll(m_events);
        m_input.beginFrame(m_events);
        if (m_window.consumeResized()) m_renderer->setPresentExtent(m_window.framebufferExtent());

        const auto now = Clock::now();
        const Tick tick = m_clock.advance(std::chrono::duration_cast<Nanos>(now - last));
        last = now;

        const float fixedDt = m_clock.fixedDtSeconds();
        for (int i = 0; i < tick.steps; ++i)
            if (hooks.fixedUpdate) hooks.fixedUpdate(*this, fixedDt);
        if (hooks.update)
            hooks.update(*this, std::chrono::duration<float>(tick.frameDt).count(), tick.alpha);

        const VkExtent2D ext = m_window.framebufferExtent();
        if (ext.width != 0 && ext.height != 0) {     // minimized: simulate, don't render
            m_renderer->render();
            // Whole frame time (input + simulation + render submit), so fps is the
            // real frame rate; GPU time comes from the deferred timestamp queries.
            m_stats.record(std::chrono::duration<double, std::milli>(tick.frameDt).count(),
                           m_renderer->gpuFrameMs());
        }
        ++frame;
    }

    RunReport r;
    r.frames = static_cast<uint64_t>(frame);
    r.readbackCopies = m_renderer->readbackCopyCount();
    r.validationMessages = m_renderer->validationMessageCount();
    r.validationEnabled = m_renderer->validationEnabled();
    r.avgFrameMs = m_stats.avgCpuMs();
    r.avgGpuMs = m_stats.avgGpuMs();
    r.fps = m_stats.fps();
    return r;
}

uint64_t Application::shutdown() {
    if (m_renderer) {
        m_renderer->shutdown();   // device and surface go before the window
        m_validationAtShutdown = m_renderer->validationMessageCount();
        m_renderer.reset();
    }
    m_window.destroy();
    return m_validationAtShutdown;
}

} // namespace ohao::runtime
