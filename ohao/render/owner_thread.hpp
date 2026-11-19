#pragma once
// The thread that may call the renderer now (step 4b, "The frame packet and the render thread"). Every public entry
// of VulkanRenderer, and those of RenderScene a world reaches, check it: a call that misses RenderThread::run is a
// data race the Vulkan layer cannot see, so it is stopped where it is made. The renderer's maker owns it first;
// RenderThread (render/render_thread.hpp) hands it to the render thread for each frame and to a run's caller for
// the run. A check costs a thread id read and a relaxed load, so it runs in every build.
#include <atomic>
#include <thread>

namespace ohao::render {

class OwnerThread {
public:
    using Id = decltype(std::this_thread::get_id());

    OwnerThread() noexcept : m_id(std::this_thread::get_id()) {}
    // `id` owns it from now on.
    void give(Id id) noexcept { m_id.store(id, std::memory_order_relaxed); }
    // The calling thread owns it from now on.
    void take() noexcept { give(std::this_thread::get_id()); }
    [[nodiscard]] bool mine() const noexcept {
        return m_id.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }
    // Logs which entry was called from which thread and aborts, unless the calling thread owns it.
    void check(const char* entry) const noexcept {
        if (!mine()) fail(entry);
    }

private:
    [[noreturn]] static void fail(const char* entry) noexcept;
    std::atomic<Id> m_id;
};

} // namespace ohao::render
