#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

// How a window presents (sub-project 9 spec, "Present mode and late input sampling"):
// `immediate` shows a frame as soon as it is done (tearing, the least latency), `mailbox`
// replaces a queued frame without tearing, `fifo` waits for the display (vsync). A surface
// without the mode asked for falls back to the next one down; fifo is always there.
enum class PresentMode : uint8_t { Fifo, Mailbox, Immediate };

[[nodiscard]] constexpr std::string_view presentModeName(PresentMode m) noexcept {
    return m == PresentMode::Immediate ? "immediate" : m == PresentMode::Mailbox ? "mailbox" : "fifo";
}
[[nodiscard]] constexpr std::optional<PresentMode> presentModeFromName(std::string_view name) noexcept {
    if (name == "immediate") return PresentMode::Immediate;
    if (name == "mailbox") return PresentMode::Mailbox;
    if (name == "fifo") return PresentMode::Fifo;
    return std::nullopt;
}

// What a windowing system hands the renderer so it can present. The renderer
// never includes a windowing header: it adds `instanceExtensions` to the
// instance and calls `createSurface` once, right after the instance exists.
struct PresentTarget {
    std::vector<const char*> instanceExtensions;
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
    VkExtent2D extent{0, 0};
    PresentMode mode{PresentMode::Mailbox};
};

} // namespace ohao
