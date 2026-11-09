#pragma once
// Fault containment (system design 4.7, step 1). Every GPU call that can report a lost device or hang goes
// through here: a lost device or a fence not signalled within kGpuTimeout becomes one report on the `gpu` log
// channel and an exit with kExitGpuLost. Nothing waits on the GPU without a limit, and no Vulkan teardown runs
// after a fault (drivers can hang in it; the OS reclaims everything).
//
// The report: kind (device_lost or hang), where, the frame, what the renderer's breadcrumbs say ran
// (setFaultContext), and VK_EXT_device_fault's description and addresses where the device has it.
//
// OHAO_INJECT_GPU_FAULT=lost:N fails the N-th frame submit as a lost device without submitting; hang:N skips
// the N-th frame submit, so its fence never signals and the wait on it times out. Neither touches the GPU.
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ohao::gpu {

inline constexpr std::chrono::milliseconds kGpuTimeout{2000};   // Windows' TDR delay
inline constexpr int kExitGpuLost = 5;   // unique across ohao (1-3) and ohao_game (1-4)

enum class FaultKind { DeviceLost, Hang };

// After device creation: the device, and whether VK_EXT_device_fault's feature was enabled on it.
void setFaultDevice(VkDevice device, bool deviceFault);
// What ran on the GPU, as a JSON value, read only when a fault is reported (the pass breadcrumbs).
void setFaultContext(std::function<std::string()> describe);
// The frame number the report names (the renderer's frame counter).
void setFaultFrame(uint64_t frame) noexcept;

[[noreturn]] void reportFault(FaultKind kind, std::string_view where);

// vkQueueSubmit with its result checked. `frame` > 0 marks a frame submit (the injection counts those).
// Returns the result for anything but a lost device (out of memory is the caller's to handle).
VkResult submit(VkQueue queue, const VkSubmitInfo& info, VkFence fence, std::string_view where, uint64_t frame = 0);
// Waits for a fence at most kGpuTimeout: a timeout is a hang, a lost device a fault; neither returns.
void waitFence(VkDevice device, VkFence fence, std::string_view where);
// Submits and waits for it and all earlier work on the queue (a fence covers everything
// submitted before it), at most kGpuTimeout. Replaces vkQueueSubmit + vkQueueWaitIdle.
void submitAndWait(VkDevice device, VkQueue queue, const VkSubmitInfo& info, std::string_view where);
// A present or acquire result: a lost device is reported, anything else handed back.
VkResult checkResult(VkResult r, std::string_view where);

} // namespace ohao::gpu
