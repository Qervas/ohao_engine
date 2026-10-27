#pragma once
// When each presented frame reached the display (sub-project 9 spec, "Latency: the definition",
// stage 5). With VK_KHR_present_id and VK_KHR_present_wait every present is tagged with an id
// and a thread asks the presentation engine when that id was presented (vkWaitForPresentKHR
// polled with a zero timeout, under the swapchain's host mutex, about every 0.25 ms while a
// present is pending). Without them, or when the engine never answers (a hidden window), a
// frame is stamped at the return of its present call and marked not waited: an underestimate.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

class Swapchain;

struct PresentStamp {
    uint64_t frame{0};                            // the renderer's submitted-frame number
    std::chrono::steady_clock::time_point at{};   // presented (waited) or the present call's return
    bool waited{false};                           // true: the presentation engine said so
};

class PresentWaiter {
public:
    using Clock = std::chrono::steady_clock;
    // A present the engine has not confirmed after this long is stamped at its call.
    static constexpr std::chrono::milliseconds kGiveUp{250};

    PresentWaiter() = default;
    ~PresentWaiter() { stop(); }
    PresentWaiter(const PresentWaiter&) = delete;
    PresentWaiter& operator=(const PresentWaiter&) = delete;

    // `wait`: vkWaitForPresentKHR from the device, or null without present wait (then no
    // thread runs and every present is stamped at its call).
    void start(VkDevice device, Swapchain& swapchain, PFN_vkWaitForPresentKHR wait);
    void stop();
    // A present call that just returned: the frame and its present id (0: untagged).
    void presented(uint64_t frame, uint64_t presentId);
    // The stamps resolved since the last call, oldest first.
    [[nodiscard]] std::vector<PresentStamp> take();
    [[nodiscard]] bool waits() const noexcept { return m_wait != nullptr; }

private:
    struct Pending {
        uint64_t frame{0}, id{0}, generation{0};
        Clock::time_point call{};
    };
    void run();
    void resolve(const Pending& p, Clock::time_point at, bool waited);

    VkDevice m_device{VK_NULL_HANDLE};
    Swapchain* m_swapchain{nullptr};
    PFN_vkWaitForPresentKHR m_wait{nullptr};
    std::mutex m_mutex;   // the queues below
    std::condition_variable m_wake;
    std::deque<Pending> m_pending;
    std::vector<PresentStamp> m_done;
    bool m_stop{false};
    std::thread m_thread;
};

} // namespace ohao
