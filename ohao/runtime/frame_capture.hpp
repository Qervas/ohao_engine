#pragma once
// render.capture's one procedure, shared by the headless RenderHost (tools/ohao) and the
// window's LiveHost, so a lockstep window capture and a headless capture of the same world
// at the same size are the same pixels by construction:
//   the world's Camera and Environment -> mode, denoiser, post effects, exposure
//   -> a temporal reset (unless the renderer is fresh) -> `frames` frames, each after
//   World::sync() (never a simulation step) -> the last frame read back on request (its
//   fence only) -> region, PNG file, preview.
// The renderer must already have the request's size; the caller checks the request against
// what its host allows.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"

namespace ohao { class VulkanRenderer; }

namespace ohao::runtime {

struct CaptureSettings {
    bool reset{true};               // false only for a renderer that has rendered nothing yet
    float defaultExposure{0.6f};    // the exposure when the request names none
    std::vector<uint8_t>* keepPixels{nullptr};   // receives the whole frame read back (tests)
};

// The frames a request renders: its own count, else deferred 1 (10 with TAA), the path
// tracer spp.
[[nodiscard]] int captureFrames(const api::RenderRequest& r) noexcept;

[[nodiscard]] api::RenderOutcome captureFrame(VulkanRenderer& renderer, ecs::World& world,
                                              const ecs::AssetResolver& assets, const api::RenderRequest& r,
                                              const CaptureSettings& settings);

// The capture's outputs from a frame's pixels: the region, the PNG file (r.path) and the
// preview (r.inlineImage). Fills `out`'s image fields and encodeMs, or adds an error.
void writeCaptureOutputs(std::span<const uint8_t> pixels, uint32_t width, uint32_t height,
                         const api::RenderRequest& r, api::RenderOutcome& out);

// E_COMMAND when r.region does not lie inside a width x height frame; empty otherwise.
[[nodiscard]] std::vector<reflect::Diagnostic> checkRegion(const api::RenderRequest& r, uint32_t width,
                                                           uint32_t height);

} // namespace ohao::runtime
