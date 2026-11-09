#pragma once
// Per-pass GPU timing (sub-project 2b spec, "Profiling first"). Each frame slot has its own
// timestamp query pool, so a frame never resets a pool another frame in flight still writes.
// A pass is bracketed by two timestamps written at BOTTOM_OF_PIPE: each marks the moment all
// earlier work has finished, so a pass's time is the work that finished inside it, passes
// never overlap, and their sum is at most the frame's span (first begin to last end).
//
// A slot's results are read when the slot comes round again, after the renderer has waited
// on its fence, so timing never stalls a frame; collectAll() reads every finished slot at
// once (a resize, a bench, render.stats). The last kRingFrames frames are kept.
//
// Breadcrumbs (system design 4.7): with VK_AMD_buffer_marker, each pass also writes the frame's number into a
// host-coherent buffer as the GPU reaches its start (TOP_OF_PIPE) and as everything before its end finishes
// (BOTTOM_OF_PIPE). After a lost device or a hang, describe() says for each frame in flight which pass last
// finished and which began without finishing.
#include "render/frame/pass_registry.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::render {

inline constexpr uint32_t kMaxTimedPasses = 64;   // per frame

struct FrameTiming {
    uint64_t frame{0};   // the renderer's frame number
    float gpuMs{0.0f};   // first pass begin to last pass end
    float cpuMs{0.0f};   // recording and submission on the CPU
    uint32_t count{0};
    std::array<uint8_t, kMaxTimedPasses> pass{};   // registry indices, in execution order
    std::array<float, kMaxTimedPasses> ms{};
};

class GpuPassTimer {
public:
    static constexpr std::size_t kRingFrames = 120;

    GpuPassTimer() = default;
    ~GpuPassTimer() { destroy(); }
    GpuPassTimer(const GpuPassTimer&) = delete;
    GpuPassTimer& operator=(const GpuPassTimer&) = delete;

    // False (and timing off) when the queue family has no timestamps. `markers`: the device has
    // VK_AMD_buffer_marker enabled (breadcrumbs on).
    bool init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamily, uint32_t slots, bool markers);
    void destroy();
    [[nodiscard]] bool enabled() const noexcept { return !m_slots.empty(); }

    // Starts a frame on `slot`: first collects the frame the slot last recorded (its fence
    // must have signalled), then resets the slot's pool in `cmd` (outside any render pass).
    void beginFrame(VkCommandBuffer cmd, uint32_t slot, uint64_t frameNumber);
    // Brackets one pass of the current frame. A name not in kPasses is not timed (logged
    // once); a begin while a pass is open closes it first.
    void begin(VkCommandBuffer cmd, std::string_view pass);
    void end(VkCommandBuffer cmd);
    // The frame was submitted: `cpuMs` is its recording and submission time. Call it only
    // for a frame that was submitted (beginFrame comes after a frame's early returns).
    void endFrame(double cpuMs);

    // Reads the slot's recorded frame into the ring, if it has one. The slot's fence must
    // have signalled (or the device be idle).
    void collect(uint32_t slot);
    // Reads every slot whose frame has finished, oldest first; `fenceSignalled(slot)` says
    // which. With the device idle, pass a function that returns true.
    template <class FenceSignalled>
    void collectAll(FenceSignalled&& fenceSignalled) {
        for (;;) {
            uint32_t oldest = UINT32_MAX;
            for (uint32_t s = 0; s < m_slots.size(); ++s)
                if (m_slots[s].pending && fenceSignalled(s) &&
                    (oldest == UINT32_MAX || m_slots[s].frame < m_slots[oldest].frame))
                    oldest = s;
            if (oldest == UINT32_MAX) return;
            collect(oldest);
        }
    }

    // What the GPU ran of each frame in flight, as JSON (a fault report's "ran"). Reads the breadcrumbs only.
    [[nodiscard]] std::string describe() const;

    // The newest frames collected, at most n, oldest first.
    [[nodiscard]] std::vector<FrameTiming> recent(std::size_t n) const;
    [[nodiscard]] const FrameTiming* latest() const noexcept;

private:
    struct Slot {
        VkQueryPool pool{VK_NULL_HANDLE};
        bool pending{false};     // a submitted frame whose results are not read yet
        uint64_t frame{0};
        float cpuMs{0.0f};
        uint32_t count{0};
        std::array<uint8_t, kMaxTimedPasses> pass{};
    };

    // Breadcrumbs: per slot, word 0 the frame (written at the top of the frame), then a pair per pass.
    static constexpr uint32_t kMarkerWords = 1 + kMaxTimedPasses * 2;
    bool initMarkers(VkPhysicalDevice physicalDevice, uint32_t slots);
    void destroyMarkers();
    void writeMarker(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, uint32_t word);
    VkBuffer m_markerBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_markerMemory{VK_NULL_HANDLE};
    const volatile uint32_t* m_markers{nullptr};
    PFN_vkCmdWriteBufferMarkerAMD m_writeMarker{nullptr};

    VkDevice m_device{VK_NULL_HANDLE};
    double m_msPerTick{0.0};
    uint64_t m_mask{~0ull};   // timestampValidBits
    std::vector<Slot> m_slots;
    int m_current{-1};        // the slot being recorded, or -1
    bool m_open{false};       // a pass is open
    std::array<FrameTiming, kRingFrames> m_ring{};
    std::size_t m_ringNext{0};
    std::size_t m_ringSize{0};
};

} // namespace ohao::render
