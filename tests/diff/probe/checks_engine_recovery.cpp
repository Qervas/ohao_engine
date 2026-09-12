// Stage 5, check 67: Gate 5 through the facade, against an engine-owned value.
#include "probe/checks_engine_recovery.hpp"

#include "probe/scene.hpp"

#include "diff/diff_renderer.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kW = 8, kH = 8;
constexpr std::uint32_t kCapacity = kW * kH;
constexpr std::uint32_t kEnvW = 64, kEnvH = 32;
constexpr std::uint32_t kBounces = 3u;
constexpr std::uint32_t kSeed = 20260828u;

// CHECK 54'S PRE-REGISTERED CRITERION, verbatim. Not re-chosen for this path.
constexpr float kThetaStar = 0.6f;
constexpr float kTheta0 = 0.3f;
constexpr std::uint32_t kIterations = 100u;
constexpr float kAlpha = 0.01f;
constexpr double kRecoveredWithin = 0.03;  // 3 * kAlpha

/// The engine's side of the contract, in PathTracer::setMaterialData's shape:
/// a CPU-side authority, and a device buffer that is a whole-array copy of it.
struct EngineValue {
    std::vector<float> cpu;
    GpuBuffer device{};

    bool create(ohao::diff::GpuProbeContext& ctx, float initial) {
        cpu.assign(1, initial);
        device = ctx.allocator().createBufferFromSpan<float>(
            std::span<const float>(cpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        return device.isValid();
    }
    /// Read the device back into the authority. This is the sync check 66
    /// showed to be load-bearing.
    void syncFromDevice(ohao::diff::GpuProbeContext& ctx) {
        ctx.allocator().invalidateBuffer(device);
        const std::span<const float> mapped = device.mappedAs<const float>(1u);
        if (!mapped.empty()) cpu[0] = mapped[0];
    }
    void destroy(ohao::diff::GpuProbeContext& ctx) { ctx.allocator().destroyBuffer(device); }
};

}  // namespace

bool checkEngineRecovery(ohao::diff::GpuProbeContext& ctx) {
    // --- The facade owns the registry and the arena.
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- init\n");
        return false;
    }
    const RegisterResult reg = renderer.registerScalarBlock("albedo", 1u);
    if (!reg.ok || !renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- register/build: %s\n",
                     reg.error.c_str());
        return false;
    }
    const DiffParam* albedoParam = renderer.registry().get(reg.id);
    const std::uint32_t kArenaFloats =
        static_cast<std::uint32_t>(renderer.registry().layout().totalBytes() / sizeof(float));
    const std::uint32_t kOffset = static_cast<std::uint32_t>(
        renderer.registry().layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

    // --- The scene, identical to check 54's.
    std::vector<float> envRgba;
    std::vector<double> envLum;
    buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    buildParityScene(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    ohao::diff::WavefrontBuffers wf;
    if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- scene setup\n");
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    // THE SCENE IS BUILT ONCE. This check renders 402 times -- 100 iterations
    // of a forward and a backward, twice over, plus the target -- and the
    // gradient probe used to upload a triangle soup and build a BLAS and a
    // TLAS for every one of them. A persistent acceleration structure is also
    // the shape an ENGINE has; handing over a span of floats per frame is not.
    ohao::diff::GpuProbeContext::OwnedScene scene;
    if (!ctx.buildOwnedScene(std::span<const float>(positions),
                             std::span<const std::uint32_t>(indices), scene)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- buildOwnedScene\n");
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontGradientOptions::PrebuiltScene sceneHandle = scene.handle();
    // AND THE PIPELINES ONCE. Five compute pipelines per render, 402 renders.
    // Built on first use and kept; the descriptor bindings are still written
    // every call, because the adjoint seed really is a new buffer each time.
    ohao::diff::GradientStages stages;

    auto render = [&](float albedo, const std::vector<float>& seed,
                      std::vector<float>& outFilm) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;
        options.adjointSeed = seed;
        options.scene = &sceneHandle;
        options.stages = &stages;
        return ctx.runWavefrontGradientProbe(
            wf, kW, kH, kBounces, camera, std::span<const float>(positions),
            std::span<const std::uint32_t>(indices), albedo, kMaterial, kSeed,
            renderer.arena(), kArenaFloats, kOffset, outFilm, options);
    };

    auto cleanup = [&]() {
        ctx.destroyOwnedStages(stages);
        ctx.destroyOwnedScene(scene);
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
    };

    // --- The target at theta*, same seed. Under CRN the loss at theta* is
    // exactly zero, so the minimiser is theta* itself.
    std::vector<float> target;
    if (!render(kThetaStar, {}, target) || target.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- target render\n");
        cleanup();
        return false;
    }

    /// One optimisation. `syncAuthority` selects the control.
    auto optimise = [&](bool syncAuthority, double& firstLoss, double& lastLoss,
                        float& finalTheta) -> bool {
        EngineValue value;
        if (!value.create(ctx, kTheta0)) return false;
        // The optimiser state lives in the arena and must start clean --
        // GradientArena::build does not zero.
        const ArenaBlock stateBlock =
            renderer.registry().layout().block(albedoParam->stateBlock);
        ctx.runImmediate([&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, renderer.arena().buffer(), stateBlock.offsetBytes,
                            static_cast<VkDeviceSize>(stateBlock.sizeBytes), 0u);
        });

        ohao::diff::DiffRenderer::AdamSettings adam;
        adam.alpha = kAlpha;
        bool ok = true;
        for (std::uint32_t it = 1; it <= kIterations && ok; ++it) {
            // 1. FORWARD, at whatever the AUTHORITY currently says.
            std::vector<float> film;
            if (!render(value.cpu[0], {}, film) || film.size() != target.size()) return false;

            // 2. LOSS and its adjoint seed, on the host: L = SUM (I - T)^2.
            double loss = 0.0;
            std::vector<float> seed(film.size(), 0.0f);
            for (std::size_t p = 0; p < film.size(); ++p) {
                const double d = static_cast<double>(film[p]) - static_cast<double>(target[p]);
                loss += d * d;
                seed[p] = static_cast<float>(2.0 * d);
            }
            if (it == 1u) firstLoss = loss;
            lastLoss = loss;

            // 3. BACKWARD. The gradient lands in the FACADE'S arena, at the
            // offset the registry assigned, and stays there.
            std::vector<float> ignored;
            if (!render(value.cpu[0], seed, ignored)) return false;

            // 4. ADAM, reading that same arena offset. No host round-trip for
            // the gradient at all -- which is what the arena holding both
            // blocks is for.
            ctx.runImmediate([&](VkCommandBuffer cmd) {
                ok = renderer.recordAdamStep(cmd, reg.id, value.device.buffer, adam, it);
            });
            if (!ok) break;

            // 5. THE SYNC. Without it the authority never changes and step 1
            // renders the same image forever.
            if (syncAuthority) value.syncFromDevice(ctx);
        }
        finalTheta = value.cpu[0];
        value.destroy(ctx);
        return ok;
    };

    double firstLoss = 0.0, lastLoss = 0.0;
    float theta = 0.0f;
    if (!optimise(true, firstLoss, lastLoss, theta)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- optimisation run\n");
        cleanup();
        return false;
    }
    const double err = std::fabs(static_cast<double>(theta) - static_cast<double>(kThetaStar));

    if (!(firstLoss > 0.0) || !std::isfinite(firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 67 -- the loss at theta_0 = %.9g is %.9g; it "
                     "must be finite and strictly positive or the run started at the answer\n",
                     static_cast<double>(kTheta0), firstLoss);
        cleanup();
        return false;
    }
    if (!(err <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 67 -- GATE 5 THROUGH THE FACADE: theta did not "
            "recover.\n"
            "  theta* = %.9g, theta_0 = %.9g, theta = %.12g after %u iterations at alpha %.9g\n"
            "  |error| = %.6g, above the PRE-REGISTERED %.6g -- which is CHECK 54'S, unchanged\n"
            "  loss %.9g -> %.9g\n"
            "  ATTRIBUTION. Check 54 runs this identical scene, criterion and objective with "
            "the HARNESS owning the registry, the arena and a host-side Adam, and is green. "
            "What differs here is only where the gradient lives (the facade's arena, read at "
            "float offset %u) and where the value lives (a CPU authority with a device buffer "
            "derived from it). So a failure is the FACADE: the arena offsets, the Adam "
            "dispatch, or the authority sync -- not the gradient, the loss or the optimiser, "
            "each of which checks 37-53 gate separately.\n",
            static_cast<double>(kThetaStar), static_cast<double>(kTheta0),
            static_cast<double>(theta), kIterations, static_cast<double>(kAlpha), err,
            kRecoveredWithin, firstLoss, lastLoss, kOffset);
        cleanup();
        return false;
    }
    if (!(lastLoss < firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 67 -- theta arrived but the loss went %.9g -> "
                     "%.9g, which did not fall\n",
                     firstLoss, lastLoss);
        cleanup();
        return false;
    }

    // --- THE CONTROL: check 66's failure, promoted from losing one value to
    // severing the loop. With no sync, every forward render reads the
    // unchanged authority.
    double staleFirst = 0.0, staleLast = 0.0;
    float staleTheta = 0.0f;
    if (!optimise(false, staleFirst, staleLast, staleTheta)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 67 -- control run\n");
        cleanup();
        return false;
    }
    const double staleErr =
        std::fabs(static_cast<double>(staleTheta) - static_cast<double>(kThetaStar));
    if (!(staleErr > kRecoveredWithin) || staleTheta != kTheta0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 67 -- THE CONTROL RECOVERED, or moved when it "
                     "could not. Without the authority sync theta must sit at exactly theta_0 = "
                     "%.9g and miss the tolerance; it is %.9g, |error| %.6g. If a run that never "
                     "updates the value it renders with still converges, this check is not "
                     "measuring the loop it claims to\n",
                     static_cast<double>(kTheta0), static_cast<double>(staleTheta), staleErr);
        cleanup();
        return false;
    }

    cleanup();
    std::printf(
        "[diff_gpu_probe] OK: check 67 -- GATE 5 THROUGH THE FACADE, AGAINST AN ENGINE-OWNED "
        "VALUE. theta* = %.4g recovered from theta_0 = %.4g to %.6g, |error| %.6g against the "
        "PRE-REGISTERED %.4g -- which is CHECK 54'S CRITERION UNCHANGED, reused so that a "
        "difference in outcome is attributable to the facade rather than to a number quietly "
        "chosen to suit the new path. Loss %.9g -> %.9g over %u iterations at alpha %.4g. WHAT "
        "MOVED relative to check 54: DiffRenderer owns the registry and the arena, the backward "
        "pass writes its gradient at the offset the REGISTRY assigned (float %u) and "
        "recordAdamStep reads it from there, so the gradient never round-trips through the "
        "host. (That offset is 0 with one parameter registered, so this check does not exercise "
        "a nonzero one -- check 65 does, at 128 and 192, and asserts they are nonzero before "
        "dispatching.) The value lives in an engine-style owner -- a CPU authority with a device "
        "buffer derived by whole-array copy, PathTracer::setMaterialData's shape. THE CONTROL "
        "IS CHECK 66'S FAILURE PROMOTED: with the authority left unsynced, every forward render "
        "reads the unchanged value, so theta sits at EXACTLY theta_0 and the loop is severed. A "
        "stale authority is not untidiness. STILL OWED: the forward and backward dispatches are "
        "still the harness's 640 lines of orchestration -- the facade owns the parameters, the "
        "arena and the optimiser, not the render -- so this proves the ENDS are connected, not "
        "the whole.\n",
        static_cast<double>(kThetaStar), static_cast<double>(kTheta0),
        static_cast<double>(theta), err, kRecoveredWithin, firstLoss, lastLoss, kIterations,
        static_cast<double>(kAlpha), kOffset);
    return true;
}

}  // namespace ohao::diff::probe
