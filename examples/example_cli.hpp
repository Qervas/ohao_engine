#pragma once

/**
 * Shared CLI helpers for OHAO examples (C++20 art bar).
 */

#include "gpu/vulkan/renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "render/rt/rt_meta.hpp"

#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao {
namespace examples {

struct RenderCliOptions {
    bool useDeferred{false};
    RenderMode rtMode{RenderMode::RTOffline};
    std::optional<DenoiseMode> denoiseOverride;
    bool flipY{false};
    bool video{false};
    bool outdoor{false};
};

/// Parse free-form trailing args: deferred | rt_realtime | rt_offline | flip | video | outdoor | --denoise=X
[[nodiscard]] inline RenderCliOptions parseRenderFlags(std::span<char*> args) {
    RenderCliOptions opts;
    for (char* raw : args) {
        if (!raw) continue;
        const std::string_view arg{raw};
        if (arg == "deferred") {
            opts.useDeferred = true;
        } else if (arg == "rt_realtime") {
            opts.rtMode = RenderMode::RTRealtime;
        } else if (arg == "rt_offline") {
            opts.rtMode = RenderMode::RTOffline;
        } else if (arg == "flip") {
            opts.flipY = true;
        } else if (arg == "video") {
            opts.video = true;
        } else if (arg == "outdoor") {
            opts.outdoor = true;
        } else if (arg.starts_with("--denoise=")) {
            opts.denoiseOverride = parseDenoiseMode(arg.substr(10));
        }
    }
    return opts;
}

[[nodiscard]] inline RenderCliOptions parseRenderFlags(int argc, char* argv[], int startIndex) {
    if (startIndex >= argc) return {};
    std::vector<char*> slice;
    slice.reserve(static_cast<std::size_t>(argc - startIndex));
    for (int i = startIndex; i < argc; ++i) slice.push_back(argv[i]);
    return parseRenderFlags(std::span<char*>{slice});
}

[[nodiscard]] inline int parseSpp(const char* s, int fallback) {
    if (!s || !*s) return fallback;
    const int v = std::atoi(s);
    return v > 0 ? v : fallback;
}

[[nodiscard]] inline std::string_view argOr(int argc, char* argv[], int index, std::string_view fallback) {
    if (index < argc && argv[index] && argv[index][0] != '\0') {
        return std::string_view{argv[index]};
    }
    return fallback;
}

/// Apply denoise override to renderer (uses path-tracer policy via setDenoiseMode).
inline void applyDenoiseOverride(VulkanRenderer& renderer, const RenderCliOptions& opts) {
    if (opts.denoiseOverride) {
        renderer.setDenoiseMode(*opts.denoiseOverride);
    }
}

[[nodiscard]] inline RenderMode resolveMode(const RenderCliOptions& opts) {
    if (opts.useDeferred) return RenderMode::Deferred;
    return opts.rtMode;
}

} // namespace examples
} // namespace ohao
