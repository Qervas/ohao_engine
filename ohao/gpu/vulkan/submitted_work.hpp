#pragma once
// GPU work submitted and not waited for (system design step 5g: a load never makes the thread that submits it wait
// for the device). A one-time command buffer goes to the queue with a fence; what it needs while it runs (a staging
// buffer, a build's scratch) is released when the fence has signalled, found by collect(), which never waits.
//
// Work on one queue runs in the order it was submitted, so whatever is submitted afterwards (a frame that draws the
// mesh, samples the texture, traces the BLAS) sees this work done: nothing has to be marked ready.
//
// What waits to be released is bounded: past kMaxBytes of it, submit() waits for the oldest (a host that loads
// scene after scene without ever rendering a frame).
#include <vulkan/vulkan.h>

#include <deque>
#include <functional>
#include <string_view>
#include <vector>

namespace ohao::gpu {

class SubmittedWork {
public:
    static constexpr VkDeviceSize kMaxBytes = 256ull << 20;

    void initialize(VkDevice device, VkQueue queue, VkCommandPool pool);
    // Waits for everything in flight and releases it. The device stays the caller's.
    void shutdown();

    // A one-time command buffer from the pool, begun.
    [[nodiscard]] VkCommandBuffer begin();
    // Ends and submits `cmd` (the fault watchdog's `where`), without waiting. `release` runs once the GPU has
    // finished it; `bytes` is what it holds until then (the bound above).
    void submit(VkCommandBuffer cmd, std::string_view where, VkDeviceSize bytes, std::function<void()> release);
    // Releases what has finished. Called every frame and before each submit.
    void collect();
    void waitAll();
    [[nodiscard]] std::size_t inFlight() const noexcept { return m_work.size(); }

private:
    struct Work {
        VkFence fence;
        VkCommandBuffer cmd;
        VkDeviceSize bytes;
        std::function<void()> release;
    };
    void finish(Work& w);

    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    VkCommandPool m_pool{VK_NULL_HANDLE};
    std::deque<Work> m_work;          // oldest first: the queue finishes them in this order
    std::vector<VkFence> m_fences;    // reset, for reuse
    VkDeviceSize m_bytes{0};
};

} // namespace ohao::gpu
