#pragma once
// The render thread (step 4b, "The frame packet and the render thread"): a thread of its own (jobs::Dedicated)
// that owns the renderer, its VkQueue and its present, and records frame N while the game thread steps and syncs
// frame N+1.
//
// - **Frames.** publish() hands the world's frame packet (render/frame_packet.hpp) over and returns: the packet is
//   double-buffered, so the world fills one while the render thread applies the other. At most one frame waits to
//   be taken; a publish while one waits blocks until the render thread takes it.
// - **Everything else** goes through run(fn): it waits for every frame published before it, then runs `fn` on the
//   calling thread while the render thread waits, and returns its result. A load (meshes, materials, textures, a
//   skinned instance), a capture, render.settings, render.stats and a readback each run so; a run inside a run
//   runs at once. The renderer's owner check (render/owner_thread.hpp) follows: the render thread owns it, a run
//   lends it to its caller.
// - **Off** (no start(), or after stop()): there is no thread, and publish() renders the packet at once on the
//   calling thread, by the same path; run(fn) is a call.
// - **Faults.** The render thread waits on the GPU only through gpu/vulkan/gpu_fault (2 s), and a fault there
//   reports and exits 5 while the game thread runs. An exception that escapes a frame is logged, and the next
//   publish() throws it on the game thread.
#include "render/frame_packet.hpp"
#include "render/owner_thread.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ohao {
class VulkanRenderer;
}
namespace ohao::jobs {
class Dedicated;
}

namespace ohao::render {

class RenderThread {
public:
    // What a published frame asks besides its packet.
    struct FrameOptions {
        bool viewChanged{false};   // a path-traced frame restarts its accumulation: the view may have moved
    };
    // What became of a published frame, in publish order.
    struct FrameReport {
        uint64_t seq{0};         // publish()'s number for it
        bool submitted{false};   // it reached the queue (not when minimized, or the swapchain was out of date)
        uint64_t frame{0};       // the renderer's submitted-frame number after it
        std::chrono::steady_clock::time_point submit{};   // when its command buffers were submitted
        float gpuMs{0.0f};       // the newest finished frame's GPU time then (VulkanRenderer::gpuFrameMs)
        uint32_t width{0};       // the renderer's size after it (a resize applies in the frame)
        uint32_t height{0};
    };

    explicit RenderThread(VulkanRenderer& renderer);
    ~RenderThread();   // stop()
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // The thread starts and owns the renderer from now on; call it on the thread that owns the renderer.
    void start();
    // After every frame published, the thread ends and the calling thread owns the renderer again.
    void stop();
    [[nodiscard]] bool running() const;

    template <class F>
    decltype(auto) run(F&& fn) {
        const Hold hold(*this);
        return std::forward<F>(fn)();
    }

    // Hands `packet` over for the next frame (it is left empty, with storage of its own) and returns the frame's
    // number (1, 2, ...). Never inside a run.
    uint64_t publish(FramePacket& packet, const FrameOptions& options = {});
    // The frames rendered since the last call, oldest first (the newest 1,024 are kept).
    [[nodiscard]] std::vector<FrameReport> takeReports();
    // Returns once the GPU has finished every frame published (a play window's late input sampling).
    void waitForGpu();

private:
    class Hold {
    public:
        explicit Hold(RenderThread& t) : m_t(t), m_held(t.acquire()) {}
        ~Hold() {
            if (m_held) m_t.release();
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;

    private:
        RenderThread& m_t;
        bool m_held;
    };
    // Waits for every frame published and holds the renderer: false when there is no thread (nothing to hold).
    bool acquire();
    void release();
    void loop();
    FrameReport renderFrame(FramePacket& packet, const FrameOptions& options, uint64_t seq);
    void report(const FrameReport& r);   // under m_mutex

    VulkanRenderer& m_renderer;
    std::unique_ptr<jobs::Dedicated> m_thread;
    mutable std::mutex m_mutex;   // everything below
    std::condition_variable m_changed;
    bool m_running{false};
    bool m_stopping{false};
    OwnerThread::Id m_renderId{};   // the render thread's
    FramePacket m_mailbox;          // published, not taken yet
    FrameOptions m_mailboxOptions;
    uint64_t m_mailboxSeq{0};
    bool m_queued{false};           // m_mailbox holds a frame
    bool m_busy{false};             // the render thread is rendering one
    FramePacket m_rendering;        // the one it renders (the render thread's alone)
    int m_holds{0};                 // runs in progress, nested, on m_holder
    OwnerThread::Id m_holder{};
    uint64_t m_published{0};
    std::deque<FrameReport> m_reports;
    std::string m_error;            // what escaped a frame on the render thread
};

} // namespace ohao::render
