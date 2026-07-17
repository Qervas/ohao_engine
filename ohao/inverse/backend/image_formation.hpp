#pragma once

// Offline image formation for inverse fit — backend-agnostic contract.
// PT wraps RenderSession; Diff will wrap DiffSession. No god objects.

#include "inverse/image_loss.hpp"
#include "inverse/quality.hpp"
#include "render/rt/denoise/denoise_types.hpp"

#include <cstdint>
#include <string_view>

namespace ohao::inverse {

enum class InverseBackend : std::uint8_t {
    PathTrace = 0,
    Diff = 1,
};

[[nodiscard]] inline const char* inverseBackendName(InverseBackend b) noexcept {
    switch (b) {
    case InverseBackend::PathTrace:
        return "pt";
    case InverseBackend::Diff:
        return "diff";
    }
    return "pt";
}

[[nodiscard]] inline InverseBackend parseInverseBackend(std::string_view s) noexcept {
    if (s == "diff" || s == "diff-ir" || s == "differentiable") return InverseBackend::Diff;
    return InverseBackend::PathTrace;
}

struct InverseRenderRequest {
    int viewIndex{0};
    RenderBudget budget{};
    uint32_t seed{1};
    DenoiseMode denoise{DenoiseMode::None};
};

/// Pluggable offline renderer used by inverse optimizers and lab eval.
class IInverseImageFormation {
public:
    virtual ~IInverseImageFormation() = default;

    [[nodiscard]] virtual InverseBackend backend() const noexcept = 0;
    [[nodiscard]] virtual bool supportsAnalyticGrads() const noexcept { return false; }

    /// Forward: apply current scene θ already set by InverseScene, render one view.
    [[nodiscard]] virtual ImageRGBA8 forward(const InverseRenderRequest& req) = 0;

    /// Optional: rebind environment map (relight eval). Default no-op.
    virtual void rebindEnv(std::string_view /*path*/) {}
};

} // namespace ohao::inverse
