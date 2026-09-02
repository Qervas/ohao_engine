// Stage 3, check 63: a world-space translation recovered through the
// projection, depth component included.
#include "probe/checks_projection_recovery.hpp"

#include "diff/geom/camera_projection.hpp"
#include "probe/coverage_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kImage = 8u;
constexpr std::uint32_t kSub = 64u;
constexpr double kLTri = 3.0;
constexpr double kLEnv = 0.5;

// THE PRE-REGISTERED CRITERION, fixed before the first run.
constexpr std::uint32_t kIterations = 250u;
constexpr float kAlpha = 0.03f;
constexpr double kRecoveredWithin = 3.0 * static_cast<double>(kAlpha);
// The control's depth parameter must be left where it started, to within a
// float's worth of nothing: a zero gradient in Adam is m = v = 0, and
// 0 / (0 + eps) is exactly 0.
constexpr double kFrozen = 1e-6;
// ||d(screen)/d(tz)|| in pixels per world unit, summed over vertices. Zero
// under an orthographic camera by construction.
constexpr double kDepthObservable = 0.25;

/// The base shape. THREE DIFFERENT DEPTHS: equal depths would make the
/// perspective divide one constant, and the depth column of the Jacobian the
/// same shape at every vertex.
///
/// THE VERTEX ORDER IS NOT ARBITRARY, and finding out why cost this check its
/// first run. The boundary pass builds an edge normal as a fixed rotation of
/// the edge direction, so which side is "inside" -- which is which of lIn and
/// lOut -- is decided by the WINDING of the edge list. A projection can
/// reverse that winding: a camera looking at the far side of a triangle sees
/// it wound the other way. The first draft of this shape projected
/// counter-clockwise where every earlier gate is clockwise, which flipped the
/// sign of the whole boundary term; tz then ASCENDED, ran from -0.9 to +5.48
/// (the camera sits at z = 6) and the loss rose 0.709 -> 5.309. The order
/// below projects clockwise, and `signedArea` below asserts it rather than
/// trusting this comment. In a real scene the facing test in the silhouette
/// pass is what settles this per edge; here there is one triangle and the
/// answer is fixed, so it is asserted instead.
const std::vector<float>& baseWorld() {
    static const std::vector<float> kBase = {-1.00f, -0.80f, 0.25f, 0.05f, 1.05f,
                                             0.10f,  1.00f,  -0.60f, -0.35f};
    return kBase;
}

/// Twice the signed area of a projected triangle; negative is the clockwise
/// winding every Stage 3 gate uses.
double signedArea(const std::vector<float>& screen) {
    if (screen.size() < 6u) return 0.0;
    const double ax = screen[2] - screen[0];
    const double ay = screen[3] - screen[1];
    const double bx = screen[4] - screen[0];
    const double by = screen[5] - screen[1];
    return ax * by - ay * bx;
}

std::vector<float> translated(const std::vector<float>& base, const std::vector<float>& t) {
    std::vector<float> out(base.size(), 0.0f);
    for (std::size_t v = 0; v < base.size() / 3u; ++v) {
        for (std::size_t c = 0; c < 3u; ++c) {
            out[v * 3u + c] = base[v * 3u + c] + t[c];
        }
    }
    return out;
}

struct Run {
    std::vector<float> theta;
    double firstLoss = 0.0;
    double lastLoss = 0.0;
    bool ok = false;
};

/// One optimisation. `orthographicPullback` selects the control: the depth
/// column of the projection Jacobian is dropped, which is exactly what a
/// screen-space-only pipeline computes.
Run runRecovery(ohao::diff::GpuProbeContext& ctx, const ohao::diff::PinholeProjection& cam,
                const std::vector<float>& target, const std::vector<float>& theta0,
                bool orthographicPullback, const char* label) {
    Run run;
    run.theta = theta0;
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};
    const std::size_t pixels = target.size();

    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> adamState(run.theta.size() * 2u, 0.0f);

    for (std::uint32_t it = 1; it <= kIterations; ++it) {
        const std::vector<float> world = translated(baseWorld(), run.theta);
        std::vector<float> screen;
        if (!cam.project(world, screen)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 63 (%s) -- a vertex left the front of the "
                         "camera at iteration %u. The projection refuses rather than clamping, "
                         "so this is a real excursion and not a silently large number\n",
                         label, it);
            return run;
        }
        const std::vector<float> image =
            renderTriangleCoverage(screen, kImage, kSub, kLTri, kLEnv);

        double loss = 0.0;
        std::vector<float> seed(pixels, 0.0f);
        for (std::size_t p = 0; p < pixels; ++p) {
            const double d = static_cast<double>(image[p]) - static_cast<double>(target[p]);
            loss += d * d / static_cast<double>(pixels);
            seed[p] = static_cast<float>(2.0 * d / static_cast<double>(pixels));
        }
        if (it == 1u) run.firstLoss = loss;
        run.lastLoss = loss;

        // dL/d(screen), from the GPU boundary pass. It is handed SCREEN
        // positions and knows nothing about the camera -- which is the
        // property that lets the projection be swapped without touching it.
        std::vector<float> screenGrad;
        if (!ctx.runBoundaryProbe(screen, edges, kImage, kImage,
                                  {static_cast<float>(kLTri), static_cast<float>(kLEnv)}, {}, seed, nullptr, 0u, 0u,
                                  screenGrad)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 63 (%s) boundary dispatch at iter %u\n",
                         label, it);
            return run;
        }

        // dL/d(world) = J_proj^T dL/d(screen).
        std::vector<float> worldGrad = cam.pullback(world, screenGrad);
        if (worldGrad.size() != world.size()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 63 (%s) -- the projection pullback returned "
                         "%zu floats for %zu world components at iteration %u\n",
                         label, worldGrad.size(), world.size(), it);
            return run;
        }

        // THE CONTROL, applied here rather than inside PinholeProjection so
        // that the thing under test is the shipped code path. Dropping the
        // depth column is what an orthographic pipeline computes.
        if (orthographicPullback) {
            for (std::size_t v = 0; v < worldGrad.size() / 3u; ++v) worldGrad[v * 3u + 2u] = 0.0f;
        }

        // dL/d(translation): the plain sum over vertices, because
        // d(world_v)/d(t) is the identity for every vertex. The trivial
        // pullback of the three -- the non-trivial one this gate is about
        // is the projection's, immediately above.
        std::vector<float> thetaGrad(3, 0.0f);
        for (std::size_t v = 0; v < worldGrad.size() / 3u; ++v) {
            for (std::size_t c = 0; c < 3u; ++c) thetaGrad[c] += worldGrad[v * 3u + c];
        }

        if (!ctx.runAdamProbe(run.theta, thetaGrad, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 63 (%s) Adam step at iter %u\n",
                         label, it);
            return run;
        }
    }
    run.ok = true;
    return run;
}

}  // namespace

bool checkProjectionRecovery(ohao::diff::GpuProbeContext& ctx) {
    const std::vector<float> star = {0.15f, -0.10f, 0.30f};
    const std::vector<float> theta0 = {-0.55f, 0.60f, -0.90f};
    static const char* const kNames[3] = {"tx", "ty", "tz"};

    ohao::diff::PinholeProjection cam;
    if (!cam.setLookAt({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}) ||
        !cam.setIntrinsics(12.0f, 12.0f, 4.0f, 4.0f)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 63 camera setup\n");
        return false;
    }

    std::vector<float> starScreen;
    if (!cam.project(translated(baseWorld(), star), starScreen)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 63 -- theta* does not project\n");
        return false;
    }
    const std::vector<float> target =
        renderTriangleCoverage(starScreen, kImage, kSub, kLTri, kLEnv);

    // --- THE PROJECTED WINDING, at both ends of the run. The boundary
    // pass's normal is a fixed rotation of the edge direction, so the
    // winding is what decides which side lIn is on; a projection that
    // reversed it would silently negate the whole boundary term. Checked at
    // theta_0 as well as theta*, because a translation that carried the
    // shape past the camera plane between them would change it midway.
    std::vector<float> startScreen;
    if (!cam.project(translated(baseWorld(), theta0), startScreen)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 63 -- theta_0 does not project\n");
        return false;
    }
    if (!(signedArea(starScreen) < 0.0) || !(signedArea(startScreen) < 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- the projected triangle winds %.6g at "
                     "theta* and %.6g at theta_0; both must be negative (clockwise), the winding "
                     "every Stage 3 gate uses. The boundary pass takes its normal as a fixed "
                     "rotation of the edge direction, so a reversed winding swaps lIn for lOut "
                     "and negates the entire boundary term -- which is gradient ASCENT, not a "
                     "small error\n",
                     signedArea(starScreen), signedArea(startScreen));
        return false;
    }

    // --- NON-VACUITY OF THE CAMERA, before anything is optimised.
    //
    // IDENTIFIABILITY IS A PROPERTY OF THE SHAPE, NOT OF ONE VERTEX. What
    // makes tz recoverable is that moving the whole shape in depth moves the
    // image, which is the NORM of d(screen)/d(tz) stacked over vertices --
    // the third Jacobian column of each. A single vertex near the optical
    // axis contributes almost nothing to it (a point dead ahead stays dead
    // ahead however far away), and that is fine as long as the others carry
    // it; a first draft of this guard demanded every vertex separately and
    // rejected a perfectly identifiable shape.
    //
    // The LATERAL norm is measured beside it, and the ratio is reported
    // rather than asserted: depth is genuinely the weakly observable
    // direction here, which is why this gate runs more iterations than
    // check 62 and why a tolerance that looks generous is the honest one.
    const std::vector<float> starWorld = translated(baseWorld(), star);
    double depthNorm2 = 0.0;
    double lateralNorm2 = 0.0;
    for (std::size_t v = 0; v < starWorld.size() / 3u; ++v) {
        const std::vector<double> j =
            cam.jacobian(starWorld[v * 3u + 0u], starWorld[v * 3u + 1u], starWorld[v * 3u + 2u]);
        if (j.size() != 6u) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 63 -- vertex %zu has no Jacobian\n",
                         v);
            return false;
        }
        depthNorm2 += j[2] * j[2] + j[5] * j[5];
        lateralNorm2 += j[0] * j[0] + j[4] * j[4];
    }
    const double depthNorm = std::sqrt(depthNorm2);
    const double lateralNorm = std::sqrt(lateralNorm2);
    if (!(depthNorm > kDepthObservable)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- ||d(screen)/d(tz)|| is %.9g, below the "
                     "pre-registered %.9g. Moving this shape in depth barely moves the image, so "
                     "tz is not identifiable in this scene and recovering it would be luck. Under "
                     "an orthographic camera this number is exactly zero\n",
                     depthNorm, kDepthObservable);
        return false;
    }

    const Run primary = runRecovery(ctx, cam, target, theta0, false, "main");
    if (!primary.ok) return false;

    std::size_t worstIdx = 0;
    double worst = 0.0;
    double startWorst = 0.0;
    for (std::size_t k = 0; k < star.size(); ++k) {
        const double err =
            std::fabs(static_cast<double>(primary.theta[k]) - static_cast<double>(star[k]));
        if (err > worst) {
            worst = err;
            worstIdx = k;
        }
        startWorst = std::max(startWorst, std::fabs(static_cast<double>(theta0[k]) -
                                                    static_cast<double>(star[k])));
    }
    const double depthErr =
        std::fabs(static_cast<double>(primary.theta[2]) - static_cast<double>(star[2]));
    const double depthStart =
        std::fabs(static_cast<double>(theta0[2]) - static_cast<double>(star[2]));

    if (!(primary.firstLoss > 0.0) || !std::isfinite(primary.firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- the loss at theta_0 is %.9g. It must be "
                     "finite and strictly positive, or the optimiser started at the answer\n",
                     primary.firstLoss);
        return false;
    }
    if (!(worst <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 63 -- A WORLD-SPACE TRANSLATION did not recover.\n"
            "  worst parameter %s: theta* = %.9g, theta_0 = %.9g, theta = %.9g\n"
            "  |error| = %.6g, above the PRE-REGISTERED %.6g (3 * alpha)\n"
            "  depth alone: started %.6g away, finished %.6g away\n"
            "  loss %.9g -> %.9g over %u iterations\n"
            "  ATTRIBUTION. Check 62 runs this same loop in screen space and is green, so the\n"
            "  boundary pass, the seed, the pullback plumbing and the Adam step are not at\n"
            "  fault; the ONE thing added here is the projection between them. The interior\n"
            "  term is still exactly zero, so it is not the interior machinery either.\n",
            kNames[worstIdx], static_cast<double>(star[worstIdx]),
            static_cast<double>(theta0[worstIdx]), static_cast<double>(primary.theta[worstIdx]),
            worst, kRecoveredWithin, depthStart, depthErr, primary.firstLoss, primary.lastLoss,
            kIterations);
        return false;
    }
    if (!(primary.lastLoss < primary.firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- the translation arrived but the loss went "
                     "%.9g -> %.9g, which did not fall\n",
                     primary.firstLoss, primary.lastLoss);
        return false;
    }

    // --- THE CONTROL: the system's own previous state. With the depth column
    // dropped, tz has EXACTLY zero gradient, Adam takes exactly no step, and
    // the parameter must sit where it started.
    const Run control = runRecovery(ctx, cam, target, theta0, true, "orthographic control");
    if (!control.ok) return false;
    const double controlMoved =
        std::fabs(static_cast<double>(control.theta[2]) - static_cast<double>(theta0[2]));
    if (!(controlMoved < kFrozen)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- with the depth column dropped, tz still "
                     "moved by %.9g. Its gradient is then exactly zero, and Adam's step at m = v "
                     "= 0 is exactly zero, so ANY movement means the depth parameter is being "
                     "driven by something other than the projection's third column\n",
                     controlMoved);
        return false;
    }
    if (!(control.lastLoss > primary.lastLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 63 -- the orthographic control finished at loss "
                     "%.9g, no worse than the perspective run's %.9g. If dropping the depth "
                     "column costs nothing, the shape's depth was not observable in this scene "
                     "and the main run's tz recovery is not attributable to the projection\n",
                     control.lastLoss, primary.lastLoss);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 63 -- A WORLD-SPACE TRANSLATION RECOVERED THROUGH THE "
        "PROJECTION, depth included. This is the Stage 3 deviation \"orthographic only (no "
        "projection Jacobian)\" closed end to end: the boundary pass still returns "
        "dL/d(screen) and still knows nothing about cameras, and the gradient reaches a WORLD "
        "parameter through J_proj^T. Worst parameter finished %.6g from theta* against a "
        "PRE-REGISTERED %.6g (3 * alpha), having started %.4g away; loss %.9g -> %.9g over %u "
        "Adam iterations at alpha = %.4g. THE DEPTH COMPONENT IS THE POINT: tz started %.4g "
        "away and finished %.6g away, and it is recoverable at all only because the third "
        "column of the projection Jacobian is nonzero -- under an orthographic camera that "
        "column is exactly zero and motion along the view direction is not merely hard to fit "
        "but UNIDENTIFIABLE. The camera is checked for that before the run, and the property "
        "checked is the SHAPE'S: ||d(screen)/d(tz)|| = %.4g px per world unit against a "
        "pre-registered %.3g, beside a lateral %.4g. Depth is the weakly observable direction "
        "-- about %.1fx weaker here -- which is a fact about perspective and not a defect, and "
        "it is why a single vertex near the optical axis, which contributes almost nothing, "
        "does not make the shape unidentifiable. THE CONTROL IS THE SYSTEM'S OWN PREVIOUS "
        "STATE -- the identical run with the "
        "depth column dropped, which is what this pipeline computed before PinholeProjection "
        "existed. It leaves tz at EXACTLY its starting value (moved %.3g; a zero gradient "
        "makes Adam's m and v zero and its step exactly zero) and finishes at loss %.9g "
        "against the perspective run's %.9g.\n",
        worst, kRecoveredWithin, startWorst, primary.firstLoss, primary.lastLoss, kIterations,
        static_cast<double>(kAlpha), depthStart, depthErr, depthNorm, kDepthObservable,
        lateralNorm, lateralNorm / depthNorm, controlMoved, control.lastLoss,
        primary.lastLoss);
    return true;
}

}  // namespace ohao::diff::probe
