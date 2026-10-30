#pragma once

#include "render/display/display.hpp"
// render.capture's one procedure, shared by the headless RenderHost (tools/ohao) and the
// window's LiveHost, so a lockstep window capture and a headless capture of the same world
// at the same size are the same pixels by construction:
//   the world's Camera and Environment -> mode, denoiser, post effects, exposure
//   -> a temporal reset (unless the renderer is fresh) -> `frames` frames, each after
//   World::sync() (never a simulation step) -> the last frame read back on request (its
//   fence only) -> region, PNG file, preview. A request for an inline image and nothing
//   else, of a deferred frame, reads back only the preview the GPU made (capture.preview).
// The renderer must already have the request's size; the caller checks the request against
// what its host allows.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "render/frame/gpu_pass_timer.hpp"

#include <optional>
#include <span>

namespace ohao { class VulkanRenderer; }

namespace ohao::runtime {

struct CaptureSettings {
    bool reset{true};               // false only for a renderer that has rendered nothing yet
    float defaultEv100{render::kDefaultEv100};   // the exposure when the request names none
    render::DisplayCurve defaultCurve{render::DisplayCurve::AgX};   // likewise the curve
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

// Marks the renderer's next frame as the capture's: only the GPU preview when an inline image
// is all `r` needs and the renderer can make it (a deferred frame), else the whole frame.
// Returns whether the preview alone was asked for. `needPixels`: the caller wants the frame.
bool requestCaptureReadback(VulkanRenderer& renderer, const api::RenderRequest& r, bool needPixels);
// Call right after that frame's render(), with renderer.framesSubmitted() as it was before
// that render(): waits for the frames queued ahead of it (queue order makes the read frame
// wait for them anyway), then reads it back into `out`: readbackMs (the read frame's fence
// and copy alone), the image fields and encodeMs, or an error. out.width and out.height are
// the frame's size; `keepPixels` receives the whole frame (a full readback only).
void readCaptureOutputs(VulkanRenderer& renderer, const api::RenderRequest& r, bool previewOnly,
                        uint64_t framesBefore, api::RenderOutcome& out, std::vector<uint8_t>* keepPixels = nullptr);

// E_COMMAND when r.region does not lie inside a width x height frame; empty otherwise.
[[nodiscard]] std::vector<reflect::Diagnostic> checkRegion(const api::RenderRequest& r, uint32_t width,
                                                           uint32_t height);

// render.stats from timed frames (oldest first): medians of the frame's GPU and CPU times
// and of each pass over the frames that ran it, passes in registry (frame) order. nullopt
// when `frames` is empty.
[[nodiscard]] std::optional<api::RenderStats> summarizeFrames(std::span<const render::FrameTiming> frames);
// The same over the renderer's newest `count` finished frames (reads every finished slot
// first; never waits on the GPU).
[[nodiscard]] std::optional<api::RenderStats> renderStats(VulkanRenderer& renderer, int count);

// render.settings on a renderer: the renderer's settings as the API's, and the API's applied
// (between frames; a GI resolution or CSM size change idles the device).
[[nodiscard]] api::RenderSettings readRenderSettings(const VulkanRenderer& renderer);
void applyRenderSettings(VulkanRenderer& renderer, const api::RenderSettings& settings);
// engine.info's gpu for the renderer's device.
[[nodiscard]] api::Host::GpuInfo gpuInfo(const VulkanRenderer& renderer);

} // namespace ohao::runtime
