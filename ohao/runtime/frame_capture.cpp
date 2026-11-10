#include "runtime/frame_capture.hpp"

#include "core/log.hpp"
#include "world_render/present.hpp"
#include "ecs/components.hpp"
#include "world_render/instantiate.hpp"
#include "ecs/scene_view.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/rt/denoise/denoise_types.hpp"
#include "render/scene/render_scene.hpp"
#include "render/vfx/vfx_renderer.hpp"
#include "runtime/image.hpp"
#include "runtime/scene_view_apply.hpp"

#include <algorithm>
#include <chrono>

namespace ohao::runtime {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

reflect::Diagnostic error(std::string code, std::string message) {
    reflect::Diagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

std::string hexId(uint64_t id) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, id >>= 4) s[static_cast<std::size_t>(i)] = kDigits[id & 15u];
    return s;
}

} // namespace

int captureFrames(const api::RenderRequest& r) noexcept {
    if (r.frames > 0) return r.frames;
    if (r.mode != "deferred") return r.spp;   // the path tracer: one sample per frame
    // Deferred: one frame, or 10 when TAA blends the history (the spec's defaults). The
    // hybrid's RT GI keeps a history too, so a converged GI needs frames given.
    return std::find(r.post.begin(), r.post.end(), "taa") != r.post.end() ? 10 : 1;
}

std::vector<reflect::Diagnostic> checkRegion(const api::RenderRequest& r, uint32_t width, uint32_t height) {
    std::vector<reflect::Diagnostic> errors;
    if (!r.region) return errors;
    const auto& g = *r.region;
    if (g[0] < 0 || g[1] < 0 || g[2] < 1 || g[3] < 1 || static_cast<int64_t>(g[0]) + g[2] > width ||
        static_cast<int64_t>(g[1]) + g[3] > height) {
        auto d = error("E_COMMAND", "region [" + std::to_string(g[0]) + ", " + std::to_string(g[1]) + ", " +
                                        std::to_string(g[2]) + ", " + std::to_string(g[3]) + "] is not inside the " +
                                        std::to_string(width) + "x" + std::to_string(height) + " frame");
        d.path = "/region";
        errors.push_back(std::move(d));
    }
    return errors;
}

namespace {

// The inline image of a preview already at its size: PNG or JPEG q85, into out.
void encodeInline(const Rgba8Image& image, const api::RenderRequest& r, api::RenderOutcome& out) {
    const bool png = r.format == "png";
    out.image = png ? encodePng(image) : encodeJpeg(image, 85);
    out.imageMime = png ? "image/png" : "image/jpeg";
    out.imageWidth = image.width;
    out.imageHeight = image.height;
    if (out.image.empty()) out.errors.push_back(error("E_RENDER", "the preview could not be encoded"));
}

} // namespace

bool requestCaptureReadback(VulkanRenderer& renderer, const api::RenderRequest& r, bool needPixels) {
    if (!r.inlineImage || !r.path.empty() || needPixels || !renderer.previewOnGpu()) {
        renderer.requestReadback();
        return false;
    }
    VulkanRenderer::PreviewRequest p;
    p.rect = r.region ? std::array<uint32_t, 4>{static_cast<uint32_t>((*r.region)[0]), static_cast<uint32_t>((*r.region)[1]),
                                                static_cast<uint32_t>((*r.region)[2]), static_cast<uint32_t>((*r.region)[3])}
                      : std::array<uint32_t, 4>{0, 0, renderer.getWidth(), renderer.getHeight()};
    p.maxSide = static_cast<uint32_t>(std::max(1, r.maxSize));
    renderer.requestPreviewReadback(p);
    return true;
}

namespace {

// The read frame's GPU time and (r.timings) its passes: its fence has signalled, so it is the
// newest finished frame.
void captureTimings(VulkanRenderer& renderer, const api::RenderRequest& r, api::RenderOutcome& out) {
    const std::vector<render::FrameTiming> frames = renderer.frameTimings(1);
    if (frames.empty()) return;
    const render::FrameTiming& f = frames.back();
    out.gpuMs = f.gpuMs;
    if (!r.timings) return;
    for (uint32_t i = 0; i < f.count && i < render::kMaxTimedPasses; ++i)
        if (f.pass[i] < static_cast<uint32_t>(render::kPassCount))
            out.timings.emplace_back(std::string(render::kPasses[f.pass[i]].name), f.ms[i]);
}

} // namespace

void readCaptureOutputs(VulkanRenderer& renderer, const api::RenderRequest& r, bool previewOnly,
                        uint64_t framesBefore, api::RenderOutcome& out, std::vector<uint8_t>* keepPixels) {
    // The frames queued ahead finish first, by queue order: their time is not this readback's.
    const auto queued = Clock::now();
    renderer.waitForSubmitted(framesBefore);
    // Once the read frame's fence has signalled (the readback below waits for it), its times.
    struct Timings {
        VulkanRenderer& renderer;
        const api::RenderRequest& r;
        api::RenderOutcome& out;
        ~Timings() { captureTimings(renderer, r, out); }
    } timings{renderer, r, out};
    const Clock::time_point submitted = Clock::now();
    OHAO_LOG(Debug, render, "capture: frames queued ahead finished in {:.2f} ms", msSince(queued));
    if (previewOnly) {
        const VulkanRenderer::Preview preview = renderer.readbackPreview();
        out.readbackMs = msSince(submitted);
        if (preview.pixels.empty()) {
            out.errors.push_back(error("E_RENDER", "the renderer produced no image"));
            return;
        }
        const auto start = Clock::now();
        const Rgba8Image image{preview.width, preview.height,
                               std::vector<uint8_t>(preview.pixels.begin(), preview.pixels.end())};
        encodeInline(image, r, out);
        OHAO_LOG(Debug, render, "capture outputs: GPU preview {}x{}, {} {:.2f} ms", preview.width, preview.height,
                 r.format == "png" ? "png" : "jpeg", msSince(start));
        out.encodeMs = msSince(start);
        return;
    }
    const std::span<const uint8_t> pixels = renderer.readback();   // a CPU denoiser runs in here
    out.readbackMs = msSince(submitted);
    if (pixels.size() != static_cast<std::size_t>(out.width) * out.height * 4) {
        out.errors.push_back(error("E_RENDER", "the renderer produced no image"));
        return;
    }
    if (keepPixels) keepPixels->assign(pixels.begin(), pixels.end());
    writeCaptureOutputs(pixels, out.width, out.height, r, out);
}

void writeCaptureOutputs(std::span<const uint8_t> pixels, uint32_t width, uint32_t height,
                         const api::RenderRequest& r, api::RenderOutcome& out) {
    const auto start = Clock::now();
    Rgba8Image image;
    std::span<const uint8_t> kept = pixels;
    uint32_t w = width, h = height;
    if (r.region) {
        image = crop(pixels, width, height, *r.region);
        kept = image.pixels;
        w = image.width;
        h = image.height;
    }
    const double cropMs = msSince(start);
    if (!r.path.empty() && !writePng(r.path, kept, w, h)) {
        out.errors.push_back(error("E_RENDER", "cannot write " + r.path));
        return;
    }
    const double fileMs = msSince(start) - cropMs;
    if (!r.path.empty()) OHAO_LOG(Debug, render, "capture outputs: PNG file {:.2f} ms", fileMs);
    if (r.inlineImage) {
        if (!r.region) image = Rgba8Image{w, h, std::vector<uint8_t>(kept.begin(), kept.end())};
        image = fitWithin(std::move(image), static_cast<uint32_t>(std::max(1, r.maxSize)));
        const double scaleMs = msSince(start) - cropMs - fileMs;
        encodeInline(image, r, out);
        OHAO_LOG(Debug, render, "capture outputs: crop {:.2f} ms, scale {:.2f} ms, {} {:.2f} ms", cropMs, scaleMs,
                 r.format == "png" ? "png" : "jpeg", msSince(start) - cropMs - fileMs - scaleMs);
        if (!out.errors.empty()) return;
    }
    out.encodeMs = msSince(start);
}

api::RenderOutcome captureFrame(VulkanRenderer& renderer, ecs::World& world, const ecs::AssetResolver& assets,
                                const api::RenderRequest& r, const CaptureSettings& settings) {
    api::RenderOutcome out;
    const auto start = Clock::now();
    out.width = renderer.getWidth();
    out.height = renderer.getHeight();
    if (auto e = checkRegion(r, out.width, out.height); !e.empty()) {
        out.errors = std::move(e);
        return out;
    }

    // As the examples: the view first, then the mode, the denoiser and post effects. The view is
    // the synced world's (a player spawns in sync, and its head is the camera).
    world.sync();
    for (const auto& w : applySceneView(renderer, ecs::resolveSceneView(world), assets).warnings)
        OHAO_LOG(Warn, render, "capture: {}", w);
    const RenderMode mode = r.mode == "rt_realtime" ? RenderMode::RTRealtime
                            : r.mode == "rt_offline" ? RenderMode::RTOffline
                                                     : RenderMode::Deferred;
    renderer.setRenderMode(mode);
    const DenoiseMode denoise = parseDenoiseMode(r.denoise);
    renderer.setDenoiseMode(denoise);
    // bounces: the path tracer's for this capture only (the referee's one-bounce comparison), the
    // profile's back when the capture returns.
    struct RestoreBounces {
        VulkanRenderer& renderer;
        std::optional<uint32_t> before;
        ~RestoreBounces() { renderer.setMaxBouncesOverride(before); }
    } restoreBounces{renderer, renderer.maxBouncesOverride()};
    if (r.bounces && mode != RenderMode::Deferred)
        renderer.setMaxBouncesOverride(static_cast<uint32_t>(*r.bounces));
    // guiding: the path tracer's for this capture only, the profile's back when it returns.
    struct RestoreGuiding {
        VulkanRenderer& renderer;
        std::optional<bool> before;
        ~RestoreGuiding() { renderer.setGuidingOverride(before); }
    } restoreGuiding{renderer, renderer.guidingOverride()};
    if (mode != RenderMode::Deferred) renderer.setGuidingOverride(r.guiding);
    // caustics: the same, for this capture only.
    struct RestoreCaustics {
        VulkanRenderer& renderer;
        std::optional<bool> before;
        ~RestoreCaustics() { renderer.setCausticsOverride(before); }
    } restoreCaustics{renderer, renderer.causticsOverride()};
    if (mode != RenderMode::Deferred) renderer.setCausticsOverride(r.caustics);
    renderer.setRenderQuality(r.quality == "reference" ? RenderQuality::Reference : RenderQuality::Realtime);
    if (mode == RenderMode::Deferred && renderer.getDeferredRenderer()) {
        if (auto* pp = renderer.getDeferredRenderer()->getPostProcessing()) {
            const auto on = [&](std::string_view e) { return std::find(r.post.begin(), r.post.end(), e) != r.post.end(); };
            pp->setBloomEnabled(on("bloom"));
            pp->setTAAEnabled(on("taa"));
        }
    }
    // Every mode displays through the post stack's EV100 and curve (the path tracer reads them).
    if (renderer.getDeferredRenderer())
        if (auto* pp = renderer.getDeferredRenderer()->getPostProcessing()) {
            pp->setAutoExposure(false);   // a capture is at a fixed EV100: repeatable
            pp->setEv100(r.ev100.value_or(settings.defaultEv100));
            pp->setDisplayCurve(r.tonemap == "neutral" ? render::DisplayCurve::Neutral
                                : r.tonemap == "agx"   ? render::DisplayCurve::AgX
                                                       : settings.defaultCurve);
        }
    // A game's HUDs only when asked (hud: true), so captures and goldens stay the scene's.
    renderer.setHud(r.hud && mode == RenderMode::Deferred ? ecs::drawHuds(world, renderer.getWidth(), renderer.getHeight())
                                                          : render::HudList{});
    const double setupMs = msSince(start);
    // A fresh renderer renders exactly as the examples do (the goldens); one that has
    // rendered starts its own accumulation, as a fresh one would.
    if (settings.reset) renderer.resetAccumulation();
    const double resetMs = msSince(start) - setupMs;

    const int frames = captureFrames(r);
    bool previewOnly = false;
    uint64_t framesBefore = 0;
    const api::CallControl* control = r.control;
    for (int i = 0; i < frames; ++i) {
        // Cancelled between whole frames: nothing is read back (the last frame's request is
        // made only when that frame is rendered).
        if (i > 0 && control && control->cancel && control->cancel->load(std::memory_order_relaxed)) {
            out.cancelled = true;
            out.frames = i;
            return out;
        }
        world.sync();   // derived state only: rendering never steps the simulation
        if (i == 0) {
            for (const entt::entity e : ecs::instantiateErrors(world)) {
                const auto& err = world.registry().get<ecs::InstantiateError>(e);
                reflect::Diagnostic d = error(err.code, "");
                d.entity = world.idOf(e);
                d.message = "entity " + hexId(d.entity) + ": " + err.message;
                out.errors.push_back(std::move(d));
            }
            if (!out.errors.empty()) return out;
        }
        if (i == frames - 1) {
            framesBefore = renderer.framesSubmitted();
            previewOnly = requestCaptureReadback(renderer, r, settings.keepPixels != nullptr);
        }
        renderer.render();
        if (control && control->progress) control->progress(static_cast<uint64_t>(i) + 1, static_cast<uint64_t>(frames));
    }
    // The last frame, exactly.
    readCaptureOutputs(renderer, r, previewOnly, framesBefore, out, settings.keepPixels);
    if (!out.errors.empty()) return out;
    // Its linear radiance (the same frame: nothing has been rendered since).
    if (!r.hdrPath.empty()) {
        const std::vector<float> linear = renderer.readbackLinear();
        if (linear.empty() || !writePfm(r.hdrPath, linear, out.width, out.height)) {
            out.errors.push_back(error("E_RENDER", linear.empty() ? "the renderer produced no linear image"
                                                                   : "cannot write " + r.hdrPath));
            return out;
        }
    }
    out.frames = frames;
    out.ms = msSince(start);
    OHAO_LOG(Debug, render, "capture: setup {:.2f} ms, reset {:.2f} ms, {} frames submitted and the frames ahead of the last "
                            "finished {:.2f} ms, readback {:.2f} ms, "
                            "encode {:.2f} ms; GPU frame (timestamps, an earlier frame) {:.2f} ms",
             setupMs, resetMs, frames, out.ms - setupMs - resetMs - out.readbackMs - out.encodeMs, out.readbackMs,
             out.encodeMs, renderer.gpuFrameMs());
    if (renderer.validationEnabled()) out.validationMessages = renderer.validationMessageCount();
    return out;
}

namespace {

double median(std::vector<float>& v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (static_cast<double>(v[n / 2 - 1]) + v[n / 2]);
}

} // namespace

std::optional<api::RenderStats> summarizeFrames(std::span<const render::FrameTiming> frames) {
    if (frames.empty()) return std::nullopt;
    api::RenderStats st;
    st.frame = frames.back().frame;
    st.frames = static_cast<int>(frames.size());
    std::vector<float> gpu, cpu;
    std::array<std::vector<float>, static_cast<std::size_t>(render::kPassCount)> pass;
    for (const render::FrameTiming& f : frames) {
        gpu.push_back(f.gpuMs);
        cpu.push_back(f.cpuMs);
        for (uint32_t i = 0; i < f.count && i < render::kMaxTimedPasses; ++i)
            if (f.pass[i] < pass.size()) pass[f.pass[i]].push_back(f.ms[i]);
    }
    st.gpuMs = median(gpu);
    st.cpuMs = median(cpu);
    for (std::size_t i = 0; i < pass.size(); ++i)
        if (!pass[i].empty()) st.passes.emplace_back(std::string(render::kPasses[i].name), median(pass[i]));
    return st;
}

api::Host::GpuInfo gpuInfo(const VulkanRenderer& renderer) {
    api::Host::GpuInfo g;
    if (renderer.getPhysicalDevice() == VK_NULL_HANDLE) return g;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(renderer.getPhysicalDevice(), &p);
    g.name = p.deviceName;
    const uint32_t v = p.driverVersion;
    g.driver = p.vendorID == 0x10DE   // NVIDIA packs 10.8.8.6
                   ? std::to_string((v >> 22) & 0x3ffu) + "." + std::to_string((v >> 14) & 0xffu)
                   : std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
                         std::to_string(VK_API_VERSION_PATCH(v));
    g.rayTracing = renderer.isRTSupported();
    g.meshShaders = renderer.meshShadersEnabled();
    return g;
}

std::optional<api::RenderStats> renderStats(VulkanRenderer& renderer, int count) {
    const std::vector<render::FrameTiming> frames = renderer.frameTimings(static_cast<std::size_t>(std::max(count, 1)));
    std::optional<api::RenderStats> st = summarizeFrames(frames);
    if (st) {
        const auto overflow = renderer.lightsOverflow();
        if (overflow && overflow->lights > 0 && overflow->frame >= frames.front().frame &&
            overflow->frame <= frames.back().frame)
            st->lightsOverflow = overflow->lights;
        st->ev100 = renderer.meteredEv100();
        if (const auto c = renderer.causticsStats()) st->caustics = api::RenderStats::Caustics{c->casters, c->clamped};
    }
    if (const auto& k = renderer.renderScene().stats().skinning; k.instances > 0)
        st->skinning = api::RenderStats::Skinning{k.instances, k.skinned, k.vertices, k.blasRefits, k.blasRebuilds};
    if (st) {
        if (DeferredRenderer* d = renderer.getDeferredRenderer()) {
            // The newest frame's effects: what the last prepare built, and the records the frame drew from.
            const render::VfxFrame& frame = renderer.renderScene().vfx();
            api::RenderStats::Vfx v;
            v.proxies = static_cast<uint32_t>(frame.smoke.size());
            v.decalsDrawn = d->decalsDrawn() ? static_cast<uint32_t>(frame.decals.size()) : 0u;
            if (const VfxRenderer* vr = d->vfxRenderer()) {
                v.particles = vr->stats().particles;
                v.objectsSorted = vr->stats().objects;
                v.tracers = vr->stats().tracers;
                v.particlesSortedMax = vr->stats().particlesSortedMax;
            }
            st->vfx = v;
        }
    }
    return st;
}

} // namespace ohao::runtime
