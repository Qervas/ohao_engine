#include "render/render_thread.hpp"

#include "core/jobs/pool.hpp"
#include "core/log.hpp"
#include "render/renderer/vulkan_renderer.hpp"

#include <cstdlib>
#include <exception>
#include <stdexcept>

namespace ohao::render {

void OwnerThread::fail(const char* entry) noexcept {
    OHAO_LOG(Error, render,
             "the renderer's {} was called from a thread that does not own it: a call outside RenderThread::run "
             "(render/render_thread.hpp) races the render thread",
             entry);
    log::flush();
    std::abort();
}

RenderThread::RenderThread(VulkanRenderer& renderer) : m_renderer(renderer) {}

RenderThread::~RenderThread() { stop(); }

bool RenderThread::running() const {
    std::lock_guard lock(m_mutex);
    return m_running;
}

void RenderThread::start() {
    if (m_thread) return;
    {
        std::lock_guard lock(m_mutex);
        m_running = true;
        m_stopping = false;
    }
    m_thread = std::make_unique<jobs::Dedicated>("ohao-render", [this] {
        {
            std::lock_guard lock(m_mutex);
            m_renderId = std::this_thread::get_id();
        }
        m_changed.notify_all();
        loop();
    });
    std::unique_lock lock(m_mutex);
    m_changed.wait(lock, [&] { return m_renderId != OwnerThread::Id{}; });
    m_renderer.owner().give(m_renderId);
}

void RenderThread::stop() {
    if (!m_thread) return;
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    m_thread->join();   // after the frame queued, if one is
    m_thread.reset();
    std::lock_guard lock(m_mutex);
    m_running = false;
    m_stopping = false;
    m_renderId = {};
    m_renderer.owner().take();
}

void RenderThread::loop() {
    for (;;) {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&] { return (m_queued && m_holds == 0) || (m_stopping && !m_queued); });
        if (!m_queued) return;
        m_mailbox.moveTo(m_rendering);
        const FrameOptions options = m_mailboxOptions;
        const uint64_t seq = m_mailboxSeq;
        m_queued = false;
        m_busy = true;
        lock.unlock();
        m_changed.notify_all();   // a publish waiting for the mailbox

        m_renderer.owner().take();
        FrameReport r{.seq = seq};
        std::string error;
        try {
            r = renderFrame(m_rendering, options, seq);
        } catch (const std::exception& e) {
            error = e.what();
            m_rendering.clear();
            OHAO_LOG(Error, render, "render thread: frame {} failed: {}", seq, error);
        }
        lock.lock();
        m_busy = false;
        report(r);
        if (!error.empty() && m_error.empty()) m_error = std::move(error);
        lock.unlock();
        m_changed.notify_all();
    }
}

RenderThread::FrameReport RenderThread::renderFrame(FramePacket& packet, const FrameOptions& options, uint64_t seq) {
    const uint64_t before = m_renderer.framesSubmitted();
    if (options.viewChanged && m_renderer.getRenderMode() != RenderMode::Deferred) m_renderer.notifyCameraChanged();
    m_renderer.render(packet);
    FrameReport r;
    r.seq = seq;
    r.frame = m_renderer.framesSubmitted();
    r.submitted = r.frame != before;
    r.submit = m_renderer.lastSubmitTime();
    r.gpuMs = m_renderer.gpuFrameMs();
    r.width = m_renderer.getWidth();
    r.height = m_renderer.getHeight();
    return r;
}

void RenderThread::report(const FrameReport& r) {
    m_reports.push_back(r);
    while (m_reports.size() > 1024) m_reports.pop_front();
}

uint64_t RenderThread::publish(FramePacket& packet, const FrameOptions& options) {
    std::unique_lock lock(m_mutex);
    if (!m_running) {
        const uint64_t seq = ++m_published;
        lock.unlock();
        const FrameReport r = renderFrame(packet, options, seq);
        lock.lock();
        report(r);
        return seq;
    }
    if (m_holds > 0 && m_holder == std::this_thread::get_id())
        throw std::logic_error("RenderThread::publish inside a run: the frame would wait for the run that waits for it");
    m_changed.wait(lock, [&] { return !m_queued || !m_error.empty(); });
    if (!m_error.empty()) throw std::runtime_error("the render thread's frame failed: " + m_error);
    packet.moveTo(m_mailbox);
    m_mailboxOptions = options;
    m_mailboxSeq = ++m_published;
    m_queued = true;
    const uint64_t seq = m_mailboxSeq;
    lock.unlock();
    m_changed.notify_all();
    return seq;
}

std::vector<RenderThread::FrameReport> RenderThread::takeReports() {
    std::lock_guard lock(m_mutex);
    std::vector<FrameReport> out(m_reports.begin(), m_reports.end());
    m_reports.clear();
    return out;
}

bool RenderThread::acquire() {
    std::unique_lock lock(m_mutex);
    if (!m_running) return false;
    const OwnerThread::Id me = std::this_thread::get_id();
    if (m_holds > 0 && m_holder == me) {
        ++m_holds;
        return true;
    }
    m_changed.wait(lock, [&] { return !m_queued && !m_busy && m_holds == 0; });
    m_holds = 1;
    m_holder = me;
    m_renderer.owner().take();
    return true;
}

void RenderThread::release() {
    {
        std::lock_guard lock(m_mutex);
        if (--m_holds > 0) return;
        m_holder = {};
        m_renderer.owner().give(m_renderId);
    }
    m_changed.notify_all();
}

void RenderThread::waitForGpu() {
    run([this] { m_renderer.waitForSubmitted(m_renderer.framesSubmitted()); });
}

} // namespace ohao::render
