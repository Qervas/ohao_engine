// Stage 1 Task 6: the four gates at two step sizes. See the header for the
// three different convergence laws the four parameters actually obey, and for
// why the pair (D(h), D(2h)) isolates the gradient's own error from the
// truncation term exactly.
#include "probe/checks_convergence.hpp"

#include "probe/fd_harness.hpp"
#include "probe/scene.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

ConvergenceFit fitConvergence(double d1, double h1, double roundoff1, double d2, double h2,
                              double roundoff2, double analytic) {
    ConvergenceFit f;
    f.h1 = h1;
    f.h2 = h2;
    f.d1 = d1;
    f.d2 = d2;
    f.analytic = analytic;
    f.roundoff1 = roundoff1;
    f.roundoff2 = roundoff2;
    f.e1 = d1 - analytic;
    f.e2 = d2 - analytic;

    const double h1sq = h1 * h1;
    const double h2sq = h2 * h2;
    const double denom = h2sq - h1sq;
    f.expectedRatio = (h1 > 0.0) ? (h2 / h1) * (h2 / h1) : 0.0;
    if (denom != 0.0) {
        // K*h1^2, from the two measurements alone -- no g in it.
        f.truncMeasured = (d2 - d1) * h1sq / denom;
        f.richardson = (h2sq * d1 - h1sq * d2) / denom;
        f.richardsonResidual = f.richardson - analytic;
        // R is linear in D1 and D2, so the roundoff bounds propagate by the
        // same coefficients, taken in absolute value.
        f.richardsonRoundoff = (h2sq * roundoff1 + h1sq * roundoff2) / std::fabs(denom);
    }
    f.observedRatio = (f.e1 != 0.0) ? f.e2 / f.e1 : 0.0;
    f.deltaImplied = f.truncMeasured - f.e1;
    return f;
}

Convergence3Fit fitConvergence3(const double* h, const double* d, double analytic) {
    Convergence3Fit f;
    for (int i = 0; i < 3; ++i) {
        f.h[i] = h[i];
        f.d[i] = d[i];
    }
    f.analytic = analytic;
    // D is a quadratic in x = h^2 through three points, so g is that
    // quadratic evaluated at x = 0 -- Lagrange, written out rather than
    // hard-coding the 2x ratio, because hActual comes back from the float
    // steps actually pushed and is only approximately 2x and 4x h1.
    const double x0 = h[0] * h[0], x1 = h[1] * h[1], x2 = h[2] * h[2];
    const double d01 = x0 - x1, d02 = x0 - x2, d12 = x1 - x2;
    if (d01 == 0.0 || d02 == 0.0 || d12 == 0.0) return f;
    f.extrapolated = d[0] * (x1 * x2) / (d01 * d02) + d[1] * (x0 * x2) / (-d01 * d12) +
                     d[2] * (x0 * x1) / (d02 * d12);
    // The same quadratic's linear and quadratic coefficients, reported at
    // h1's scale so they are directly comparable to the tolerance.
    const double c = -(d[0] * (x1 + x2) / (d01 * d02) + d[1] * (x0 + x2) / (-d01 * d12) +
                       d[2] * (x0 + x1) / (d02 * d12));
    const double e = d[0] / (d01 * d02) + d[1] / (-d01 * d12) + d[2] / (d02 * d12);
    f.truncH2 = c * x0;
    f.truncH4 = e * x0 * x0;
    f.deltaImplied = f.extrapolated - analytic;
    return f;
}

namespace {

// ONE verdict body for all four gates. Two assertions, both scaled by the
// same pre-registered tolerance so that neither can be relaxed for one
// parameter without relaxing it for every parameter:
//
//   1. THE GRADIENT. |deltaImplied| <= tol*|g|. deltaImplied is exactly the
//      gradient's error under the h^2 model (header derivation), with the
//      truncation term removed rather than bounded.
//   2. THE LAW. Whether an h^2 term is PRESENT is not a free parameter here
//      -- it is decided by the analytic form of J, stated per case by the
//      caller. A case whose truncation should vanish must measure below the
//      tolerance; a case whose truncation should be there must measure a
//      stated factor ABOVE it. Both directions fail, which is what stops
//      this from being a check that only ever confirms.
bool gateFit(const char* checkName, const char* caseName, const ConvergenceFit& f,
             bool truncationExpected, double& worstMargin) {
    const double scale = std::fabs(f.analytic);
    // One relative tolerance; the two-point gates all run at caseScale 1.
    const double tol = kConvergenceRelTol * scale;
    bool ok = true;

    if (!(scale > 0.0) || !std::isfinite(f.d1) || !std::isfinite(f.d2) ||
        !std::isfinite(f.analytic)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s on %s -- D(h) %.9g, D(2h) %.9g, g %.9g. All "
                     "three must be finite and the gradient nonzero, or the two laws below "
                     "compare nothing\n",
                     checkName, caseName, f.d1, f.d2, f.analytic);
        return false;
    }

    if (truncationExpected) {
        const double need = kTruncationPresentFactor * tol;
        if (!(std::fabs(f.truncMeasured) >= need)) {
            std::fprintf(
                stderr,
                "[diff_gpu_probe] FAIL: %s on %s REFUSES TO CLAIM A VERDICT: the h^2 term this "
                "case exists to observe measures %.6g, which does not clear %.6g (%.3g x the "
                "%.3g tolerance). J is not a low-order polynomial in this parameter, so the "
                "truncation is genuinely there; measuring it at the floor means h is too small "
                "for it to be seen, and 'the error falls as h^2' would be confirmed on a "
                "quantity indistinguishable from zero\n",
                checkName, caseName, f.truncMeasured, need, kTruncationPresentFactor,
                kConvergenceRelTol);
            ok = false;
        }
    } else if (!(std::fabs(f.truncMeasured) <= tol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: %s on %s -- J IS NOT THE FUNCTION OF THIS PARAMETER IT IS "
            "DOCUMENTED TO BE.\n"
            "  D(h)  = %.12g at h = %.9g\n"
            "  D(2h) = %.12g at h = %.9g\n"
            "  the h^2 term they imply is %.6g, and it must be <= %.6g (%.3g of |g| = %.9g)\n"
            "  A central difference is EXACT on this parameter -- linear, or quadratic and "
            "below -- so D(2h) and D(h) are the same number up to roundoff and this "
            "difference should not exist. It existing means J has acquired a dependence the "
            "derivation says it does not have: the parameter has reached a sampling decision, "
            "a density or a throughput. That is the common-random-number precondition "
            "failing, and it fails here on J itself rather than on the path geometry the "
            "trace-mismatch count watches\n",
            checkName, caseName, f.d1, f.h1, f.d2, f.h2, f.truncMeasured, tol,
            kConvergenceRelTol, f.analytic);
        ok = false;
    }

    const double margin = std::fabs(f.deltaImplied) / tol;
    if (margin > worstMargin) worstMargin = margin;
    if (!(std::fabs(f.deltaImplied) <= tol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: %s on %s -- THE ANALYTIC GRADIENT IS WRONG BY %.6g.\n"
            "  D(h)  = %.12g at h = %.9g   (error against g: %.6g)\n"
            "  D(2h) = %.12g at h = %.9g   (error against g: %.6g)\n"
            "  gradient scattered into the arena = %.12g\n"
            "  the h^2 term the PAIR implies is %.6g; the error at h is %.6g; the difference\n"
            "  between those two is the gradient's own error, and it is %.6g -- above the\n"
            "  %.3g x |g| = %.6g this gate allows.\n"
            "  Richardson extrapolant (h^2 term removed) = %.12g, residual %.6g\n"
            "  This is NOT the single-h comparison checks 37/39/40/42/45 make. Those bound\n"
            "  |D(h) - g| by roundoff + truncation, and a gradient wrong by less than the\n"
            "  truncation term passes them while looking exact. Here the truncation is\n"
            "  MEASURED from the second step size and subtracted, so what is left is the\n"
            "  error itself. A gate this tight rejecting while check 37/39/40/42/45 passes\n"
            "  means the gradient is wrong by an amount between the two tolerances -- which\n"
            "  is precisely the band a single step size cannot see.\n",
            checkName, caseName, f.deltaImplied, f.d1, f.h1, f.e1, f.d2, f.h2, f.e2, f.analytic,
            f.truncMeasured, f.e1, f.deltaImplied, kConvergenceRelTol, tol, f.richardson,
            f.richardsonResidual);
        ok = false;
    }
    return ok;
}

void reportFit(const char* caseName, const ConvergenceFit& f, bool truncationExpected) {
    const double tol = kConvergenceRelTol * std::fabs(f.analytic);
    if (truncationExpected) {
        std::printf(
            "    %-52s D(h) %.9g  D(2h) %.9g  g %.9g\n"
            "        h^2 term %.6g (%.4g x tolerance, so the law is observable); "
            "e(2h)/e(h) = %.5f against %.0f predicted\n"
            "        gradient error |T - e(h)| = %.4g, which is %.3g of the %.4g allowed "
            "(%.2g of |g|)\n",
            caseName, f.d1, f.d2, f.analytic, f.truncMeasured,
            std::fabs(f.truncMeasured) / tol, f.observedRatio, f.expectedRatio,
            std::fabs(f.deltaImplied), std::fabs(f.deltaImplied) / tol, tol,
            std::fabs(f.deltaImplied) / std::fabs(f.analytic));
    } else {
        std::printf(
            "    %-52s D(h) %.9g  D(2h) %.9g  g %.9g\n"
            "        h^2 term %.6g -- IDENTICALLY ZERO by the derivation, and measured at "
            "%.3g of the %.4g tolerance\n"
            "        gradient error |T - e(h)| = %.4g, which is %.3g of the %.4g allowed "
            "(%.2g of |g|)\n",
            caseName, f.d1, f.d2, f.analytic, f.truncMeasured, std::fabs(f.truncMeasured) / tol,
            tol, std::fabs(f.deltaImplied), std::fabs(f.deltaImplied) / tol, tol,
            std::fabs(f.deltaImplied) / std::fabs(f.analytic));
    }
}

// The three-point verdict, for the two parameters where J is not a low-order
// polynomial. Same two assertions and the SAME pre-registered tolerance as
// gateFit -- what differs is only that the h^4 term is fitted and removed
// rather than charged to the gradient.
bool gateFit3(const char* checkName, const char* caseName, const Convergence3Fit& f,
              double& worstMargin) {
    const double scale = std::fabs(f.analytic);
    const double tol = kConvergenceRelTol * scale * f.caseScale;
    bool ok = true;
    if (!(scale > 0.0) || !std::isfinite(f.extrapolated)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s on %s -- extrapolant %.9g against g %.9g; both "
                     "must be finite and the gradient nonzero\n",
                     checkName, caseName, f.extrapolated, f.analytic);
        return false;
    }
    // THE LAW IS OBSERVABLE. Same refusal as the two-point gate: an
    // asymptotic h^2 confirmed at a step where the h^2 term sits at the
    // measurement floor is not confirmed at all.
    const double need = kTruncationPresentFactor * tol;
    if (!(std::fabs(f.truncH2) >= need)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s on %s REFUSES TO CLAIM A VERDICT: the h^2 term "
                     "measures %.6g, which does not clear %.6g (%.3g x the %.3g tolerance). J is "
                     "not polynomial in this parameter, so the truncation is genuinely there; "
                     "measuring it at the floor means h is too small for the law to be seen\n",
                     checkName, caseName, f.truncH2, need, kTruncationPresentFactor,
                     kConvergenceRelTol);
        ok = false;
    }
    // THE TWO CALLS AGREE AT THE STEP THEY SHARE. Both render D(2h)
    // independently; a disagreement above the floor means the pair is not two
    // readings of one curve and nothing fitted through them means anything.
    if (!(std::fabs(f.crossCheck) <= tol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s on %s -- the two calls disagree by %.6g at the "
                     "step 2h they share, which must be EXACTLY zero and is allowed %.6g here "
                     "only so the comparison is not a bare float equality. The forward film is "
                     "deterministic (check 36), so two renders at the same parameter value and "
                     "the same seed are bit-identical; a difference means the two calls did not "
                     "land on the same step, and the three points are not three readings of one "
                     "curve\n",
                     checkName, caseName, f.crossCheck, tol);
        ok = false;
    }
    const double margin = std::fabs(f.deltaImplied) / tol;
    if (margin > worstMargin) worstMargin = margin;
    if (!(std::fabs(f.deltaImplied) <= tol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: %s on %s -- THE ANALYTIC GRADIENT IS WRONG BY %.6g.\n"
            "  D(h)  = %.12g at h  = %.9g\n"
            "  D(2h) = %.12g at 2h = %.9g\n"
            "  D(4h) = %.12g at 4h = %.9g\n"
            "  gradient scattered into the arena = %.12g\n"
            "  fitted through (h^2, D) as a quadratic and evaluated at h^2 = 0: %.12g\n"
            "  the fit charges %.6g to the h^2 term and %.6g to the h^4 term at h; what is\n"
            "  left over is the gradient's own error, %.6g, above the %.3g x |g| = %.6g\n"
            "  this gate allows.\n"
            "  THREE points, not two, and that matters here: J is not polynomial in this\n"
            "  parameter, so a two-point fit would charge 4*E*h^4 to the gradient and could\n"
            "  reject a correct adjoint. If the h^4 term above is small relative to this\n"
            "  error, the error is real; if they are comparable, suspect the step instead.\n",
            checkName, caseName, f.deltaImplied, f.d[0], f.h[0], f.d[1], f.h[1], f.d[2], f.h[2],
            f.analytic, f.extrapolated, f.truncH2, f.truncH4, f.deltaImplied,
            kConvergenceRelTol, tol);
        ok = false;
    }
    return ok;
}

// The scene every gate below runs on: check 37's, unchanged. Bundled so that
// four gates cannot drift into four slightly different scenes -- the whole
// point of a convergence gate is that it is a sharper reading of the SAME
// measurement the single-h check made.
struct GateScene {
    static constexpr uint32_t kW = 64;
    static constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
    static constexpr uint32_t kCapacity = kW * kH;
    static constexpr uint32_t kEnvW = 64;
    static constexpr uint32_t kEnvH = 32;
    static constexpr double kFilmRelativeEps = 2e-6;

    std::vector<float> positions;
    std::vector<uint32_t> indices;
    std::vector<float> envRgba;
    std::vector<double> envLum;
    ohao::diff::WavefrontGenerateCamera camera{};
    ohao::EnvCDF cdf;
    ohao::diff::WavefrontBuffers wf;
    bool built{false};

    bool build(ohao::diff::GpuProbeContext& ctx, const char* checkName) {
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
        buildParityScene(positions, indices);
        camera = parityCamera();
        cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!cdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s EnvCDF::build produced no CDF\n",
                         checkName);
            return false;
        }
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s buffers build / env CDF upload\n",
                         checkName);
            wf.destroy(ctx.allocator());
            return false;
        }
        built = true;
        return true;
    }
    void destroy(ohao::diff::GpuProbeContext& ctx) {
        if (built) wf.destroy(ctx.allocator());
        built = false;
    }
};

}  // namespace

// ===========================================================================
// 46. THE ALBEDO GATE -- both laws at once
// ===========================================================================
bool checkAlbedoConvergence(ohao::diff::GpuProbeContext& ctx) {
    // Check 37's scene, seed and material exactly. What differs is the STEP,
    // and deliberately: check 37's h = 2^-7 minimises |D(h) - g|, which
    // balances truncation against roundoff and therefore keeps truncation
    // SMALL. This gate wants the opposite -- the h^2 term is the thing being
    // measured, and Richardson removes it afterwards either way, so a larger
    // h makes the law more visible at no cost in accuracy. h = 2^-5 puts the
    // 3-bounce truncation ~300x above the measurement floor where 2^-7 put
    // it ~20x. There is no accuracy penalty to pay for that here, and this is
    // the one parameter where that is EXACTLY rather than approximately true:
    // J is a cubic in the albedo, so the h^2 model has no O(h^4) remainder to
    // grow, at any h. a +/- 2h stays inside [0.475, 0.725] and well away from
    // the [0,1] clamp.
    constexpr float kAlbedo = 0.6f;
    constexpr float kStep = 0.03125f;  // 2^-5 -- derived above
    constexpr uint32_t kGradientSeed = 20260828u;
    constexpr uint32_t kBounceCounts[3] = {1u, 2u, 3u};
    // WHICH LAW EACH BOUNCE COUNT OBEYS -- read off the polynomial degree,
    // not off the measurement. J(a) = SUM_{n=1..B} K_n a^n and a central
    // difference is exact through n = 2, so B = 1 and B = 2 have NO h^2 term
    // and B = 3 has exactly one.
    constexpr bool kTruncationExpected[3] = {false, false, true};

    ohao::diff::ParamRegistry gradReg;
    const auto regAlbedo = gradReg.registerScalarBlock("albedo", 1);
    if (!regAlbedo.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 46 registry setup: %s\n",
                     regAlbedo.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = gradReg.find("albedo");
    if (albedoParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 46 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 46 gradient arena build\n");
        return false;
    }
    const uint32_t kGradArenaFloats =
        static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
    const uint32_t kGradAlbedoOffset = static_cast<uint32_t>(
        gradReg.layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

    GateScene scene;
    if (!scene.build(ctx, "check 46")) {
        gradArena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kGradMaterial{1.0f, 0.0f, 0.0f};
    ConvergenceFit fits[3]{};
    char names[3][64]{};
    bool ok = true;
    double worstMargin = 0.0;

    for (std::size_t i = 0; i < 3; ++i) {
        const uint32_t bounces = kBounceCounts[i];
        CrnFdMeasurement m1{};
        CrnFdMeasurement m2{};
        const bool okH = measureCrnAlbedoGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, bounces, scene.camera, scene.positions,
            scene.indices, kAlbedo, kStep, kGradMaterial, kGradientSeed, gradArena,
            albedoParam->gradBlock, kGradArenaFloats, kGradAlbedoOffset,
            GateScene::kFilmRelativeEps, m1);
        const bool ok2H = measureCrnAlbedoGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, bounces, scene.camera, scene.positions,
            scene.indices, kAlbedo, 2.0f * kStep, kGradMaterial, kGradientSeed, gradArena,
            albedoParam->gradBlock, kGradArenaFloats, kGradAlbedoOffset,
            GateScene::kFilmRelativeEps, m2);
        if (!okH || !ok2H) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 46 measurement failed at %u bounce(s)\n",
                         bounces);
            scene.destroy(ctx);
            gradArena.destroy(ctx.allocator());
            return false;
        }
        fits[i] = fitConvergence(m1.finiteDiff, m1.hActual, m1.roundoffBound, m2.finiteDiff,
                                 m2.hActual, m2.roundoffBound, m1.analytic);
        std::snprintf(names[i], sizeof(names[i]), "%u bounce(s), %s", bounces,
                      kTruncationExpected[i] ? "h^2 term expected" : "truncation exactly zero");
        if (!gateFit("check 46", names[i], fits[i], kTruncationExpected[i], worstMargin)) {
            ok = false;
        }
    }

    scene.destroy(ctx);
    gradArena.destroy(ctx.allocator());
    if (!ok) return false;

    std::printf(
        "[diff_gpu_probe] OK: check 46 -- THE ALBEDO GATE AT TWO STEP SIZES, and it carries "
        "TWO CONTRARY LAWS with one instrument. J(a) = SUM_{n=1..B} K_n a^n exactly for a pure "
        "Lambertian at B bounces, and a central difference is exact through n = 2 -- so at 1 "
        "and 2 bounces the truncation is IDENTICALLY ZERO and at 3 it is EXACTLY K_3*h^2 with "
        "no higher-order remainder. Both are asserted, in opposite directions, against the one "
        "pre-registered tolerance %.3g x |g|: the first two must measure an h^2 term BELOW it, "
        "the third at least %.0fx ABOVE it. A bug that removed real curvature and a bug that "
        "added spurious curvature therefore fail different cases of the same check. Seed %u, "
        "512 paths at one sample per pixel, h = 2^-5 = %.9g and 2h (LARGER than check 37's "
        "2^-7: that step minimises the single-h error, which means keeping truncation small, "
        "and this gate measures the truncation instead -- with no accuracy cost, since a cubic "
        "has no O(h^4) term at any h).\n",
        kConvergenceRelTol, kTruncationPresentFactor, kGradientSeed,
        static_cast<double>(kStep));
    for (std::size_t i = 0; i < 3; ++i) reportFit(names[i], fits[i], kTruncationExpected[i]);
    std::printf(
        "  Worst gradient error %.4g of tolerance. WHAT THIS BUYS OVER CHECK 37: that check "
        "bounds |D(h) - g| by roundoff + truncation and passes anything inside 2.7e-4 of the "
        "gradient; here the truncation is measured and subtracted rather than added to a "
        "bound, and all three cases are bound by the RELATIVE term, so this gate rejects at "
        "%.3g -- 27x tighter. An adjoint scaled by 1.0001 passes check 37 (0.219 against its "
        "0.589 bound) and is rejected here by a factor of ten. That band is what a single step "
        "size cannot see. The claim is specific to this gate: checks 47-49 are bound by the "
        "floor term instead, where the gain is the 5x fraction plus whatever the removed "
        "truncation was worth.\n",
        worstMargin, kConvergenceRelTol);
    return true;
}

// ===========================================================================
// 47. THE ROUGHNESS/METALLIC GATE -- the only asymptotic h^2 of the four
// ===========================================================================
bool checkGgxConvergence(ohao::diff::GpuProbeContext& ctx) {
    constexpr float kAlbedo = 0.6f;
    constexpr uint32_t kGgxSeed = 20260829u;
    constexpr uint32_t kParamRoughness = 1u;
    constexpr uint32_t kParamMetallic = 2u;

    struct GgxCase {
        const char* name;
        ohao::diff::WavefrontScatterMaterial material;
        uint32_t param;
        /// The BASE step of this gate's three-point ladder: D is measured at
        /// h, 2h and 4h. Check 39/40's step is `singleH` and this is a
        /// MULTIPLE of it, never smaller -- see the table's comment.
        float h;
        float singleH;
        uint32_t bounces;
        /// (0.60/r)^2 for roughness, 1 for metallic -- the header's floor
        /// law, evaluated here so the table shows what each case is allowed.
        double caseScale;
    };
    // THE STEPS ARE CHECK 39/40's, MULTIPLIED -- and the multiplier is the
    // whole difference between a single-h gate and a convergence gate.
    //
    // Check 39/40 chose h to MINIMISE |D(h) - g|, which balances truncation
    // against roundoff and therefore lands where truncation is as SMALL as it
    // can usefully be. This gate measures the truncation, so that step is the
    // worst available: at check 39's h the broad dielectric's h^2 term comes
    // out at 0.0049 against a floor of 0.0072, i.e. BELOW it, and the gate
    // correctly refuses to claim a verdict. Multiplying h multiplies the h^2
    // term by the square and costs nothing here, because the fit removes it.
    //
    // THREE CONSTRAINTS SET EACH MULTIPLIER, and they are stated rather than
    // folded into one number because they bind on different cases:
    //   (a) the ladder reaches theta +/- 4h, which must stay inside the
    //       parameter's valid domain -- pbr_unpack.glsl's 0.01 roughness
    //       floor, unpackHitPbr's [0,1] metallic clamp;
    //   (b) 4h must stay small against the LOBE WIDTH alpha = r^2, since the
    //       expansion is in h and its remainder is governed by how much alpha
    //       moves: the relative shift is 2h/r, held under ~0.1 everywhere;
    //   (c) the h^2 term must clear kTruncationPresentFactor x the tolerance,
    //       or the gate refuses a verdict rather than confirming a law it
    //       cannot see.
    // The broad dielectric takes 8x: alpha = 0.36 there, the widest lobe of
    // the six, so (b) is slack and (c) is what binds.
    //
    // TWO CASES CANNOT TAKE THE FULL 4x, and the binding constraint is stated
    // per case rather than folded into one number:
    //   * near-specular roughness takes NO multiplier at all -- 1x. Two
    //     constraints bind at once. r = 0.04 sits against pbr_unpack.glsl's
    //     0.01 floor and the ladder reaches r - 4h, which at 4x would be
    //     0.0088, BELOW it: exactly the one-sided-derivative hazard check
    //     39's own precondition exists to refuse. And the step must stay
    //     small against the LOBE WIDTH, not just against the domain: alpha =
    //     r^2 = 1.6e-3 here, so a step of 2^-8 moves alpha by 20% and the
    //     expansion in h stops being an expansion. It needs no multiplier
    //     anyway -- its h^2 term is the largest of the six by two orders of
    //     magnitude, so the law is observable at check 39's own step.
    //   * both metallic cases reach m - 4h and m + 4h against the [0,1] clamp
    //     unpackHitPbr applies; at 2x the widest excursion is [0.575, 0.825].
    const GgxCase cases[6] = {
        {"roughness: dielectric, broad lobe (r=0.60, m=0.00)",
         {0.60f, 0.00f, 1.0f}, kParamRoughness, 0.03125f, 0.00390625f, 2u, 1.0},
        {"roughness: mixture (r=0.35, m=0.50)",
         {0.35f, 0.50f, 1.0f}, kParamRoughness, 0.015625f, 0.00390625f, 3u, 2.939},
        {"roughness: conductor, glossy (r=0.10, m=1.00)",
         {0.10f, 1.00f, 1.0f}, kParamRoughness, 0.0078125f, 0.0009765625f, 2u, 36.0},
        {"roughness: conductor, NEAR-SPECULAR (r=0.04, m=1.00)",
         {0.04f, 1.00f, 1.0f}, kParamRoughness, 0.00390625f, 0.001953125f, 2u, 225.0},
        {"metallic: broad lobe, mid metallic (r=0.60, m=0.35)",
         {0.60f, 0.35f, 1.0f}, kParamMetallic, 0.03125f, 0.015625f, 2u, 1.0},
        {"metallic: glossy, high metallic (r=0.20, m=0.70)",
         {0.20f, 0.70f, 1.0f}, kParamMetallic, 0.03125f, 0.015625f, 3u, 1.0},
    };

    // TWO parameters, and the second is never written by anything. Checks 38
    // and 43 make this assertion for the albedo and the emission and check 44
    // for the texture; roughness and metallic were the one family with no
    // null test, so it is made here, over the same whole arena and by the
    // same exact-float comparison.
    ohao::diff::ParamRegistry gradReg;
    const auto reg = gradReg.registerScalarBlock("ggx_param", 1);
    const auto regUnused = gradReg.registerScalarBlock("unused_ggx_scalar", 1);
    if (!reg.ok || !regUnused.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 47 registry setup: %s %s\n",
                     reg.error.c_str(), regUnused.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* ggxParam = gradReg.find("ggx_param");
    const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_ggx_scalar");
    if (ggxParam == nullptr || unusedParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 47 registered params not found\n");
        return false;
    }
    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 47 gradient arena build\n");
        return false;
    }
    const uint32_t kArenaFloats =
        static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
    const uint32_t kOffset = static_cast<uint32_t>(
        gradReg.layout().block(ggxParam->gradBlock).offsetBytes / sizeof(float));

    GateScene scene;
    if (!scene.build(ctx, "check 47")) {
        gradArena.destroy(ctx.allocator());
        return false;
    }

    Convergence3Fit fits[6]{};
    bool ok = true;
    double worstMargin = 0.0;
    for (std::size_t i = 0; i < 6; ++i) {
        const GgxCase& c = cases[i];
        // TWO CALLS, THREE STEPS. measureDetachedGgxGradient renders five
        // times and returns the central difference at BOTH its step and twice
        // it, so a call at h yields (D(h), D(2h)) and a call at 2h yields
        // (D(2h), D(4h)). The step 2h is therefore measured TWICE, by
        // independent renders -- gateFit3 uses that as a direct read of the
        // measurement floor rather than discarding it.
        GgxFdMeasurement lo{};
        GgxFdMeasurement hi{};
        const bool okLo = measureDetachedGgxGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, c.bounces, scene.camera, scene.positions,
            scene.indices, kAlbedo, c.material, c.param, c.h, /*freezeSampling=*/true, kGgxSeed,
            gradArena, ggxParam->gradBlock, kArenaFloats, kOffset, GateScene::kFilmRelativeEps,
            lo);
        const bool okHi = measureDetachedGgxGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, c.bounces, scene.camera, scene.positions,
            scene.indices, kAlbedo, c.material, c.param, 2.0f * c.h, /*freezeSampling=*/true,
            kGgxSeed, gradArena, ggxParam->gradBlock, kArenaFloats, kOffset,
            GateScene::kFilmRelativeEps, hi);
        if (!okLo || !okHi) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 47 measurement failed on %s\n",
                         c.name);
            scene.destroy(ctx);
            gradArena.destroy(ctx.allocator());
            return false;
        }
        const double h3[3] = {lo.hActual, lo.hActual2, hi.hActual2};
        const double d3[3] = {lo.finiteDiff, lo.finiteDiff2h, hi.finiteDiff2h};
        fits[i] = fitConvergence3(h3, d3, lo.analytic);
        fits[i].crossCheck = hi.finiteDiff - lo.finiteDiff2h;
        fits[i].roundoffBound = lo.roundoffBound;
        fits[i].caseScale = c.caseScale;
        if (!gateFit3("check 47", c.name, fits[i], worstMargin)) ok = false;
    }

    // --- THE NULL TEST, on the arena the LAST case left behind. Every float
    // outside the one element the scatter is allowed to touch must be
    // EXACTLY 0.0f, compared as a float and not through a tolerance: the
    // unused parameter's two blocks, this parameter's own Adam state, and the
    // 256-byte alignment padding -- which is where a mis-computed element
    // index is likeliest to land.
    const std::vector<float> wholeArena = gradArena.readbackAll(ctx.allocator());
    std::size_t stray = 0;
    std::size_t firstStray = 0;
    double firstStrayValue = 0.0;
    if (wholeArena.size() != static_cast<std::size_t>(kArenaFloats)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 47 -- the whole-arena readback returned %zu "
                     "floats, expected %u. A null test over the wrong number of floats is not a "
                     "null test\n",
                     wholeArena.size(), kArenaFloats);
        ok = false;
    } else {
        for (std::size_t k = 0; k < wholeArena.size(); ++k) {
            if (k == static_cast<std::size_t>(kOffset)) continue;
            if (wholeArena[k] != 0.0f) {
                if (stray == 0) {
                    firstStray = k;
                    firstStrayValue = static_cast<double>(wholeArena[k]);
                }
                ++stray;
            }
        }
        if (stray != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 47 -- %zu of the %u arena floats outside "
                         "the roughness/metallic gradient element are nonzero; the first is "
                         "float %zu at %.9g. The scene depends on ONE registered parameter, so "
                         "every other float -- the unused parameter's gradient and Adam state, "
                         "this parameter's own Adam state, and the alignment padding -- must be "
                         "exactly 0.0f\n",
                         stray, kArenaFloats, firstStray, firstStrayValue);
            ok = false;
        }
    }

    scene.destroy(ctx);
    gradArena.destroy(ctx.allocator());
    if (!ok) return false;

    std::printf(
        "[diff_gpu_probe] OK: check 47 -- THE ROUGHNESS/METALLIC GATE, AND THE ONE PLACE THE "
        "STAGE PLAN'S 'TWO STEP SIZES' IS NOT ENOUGH. J is smooth but NOT polynomial in either "
        "parameter, so D(h) = g + C*h^2 + E*h^4 + ... with E genuinely nonzero, and a two-point "
        "fit reports the gradient's error CONTAMINATED by 4*E*h^4. Measured on these six cases "
        "that contamination reaches several times the error itself, so a two-point gate here "
        "would reject a CORRECT adjoint and name the wrong cause -- which is why this gate fits "
        "THREE points (h, 2h, 4h) and leaves O(h^6). The other three gates need only two, and "
        "provably so: J is a polynomial of degree <= 3 in the albedo, the emission and an "
        "emission texel, so E is exactly zero there. HOW MANY STEP SIZES A GATE NEEDS IS SET BY "
        "THE POLYNOMIAL DEGREE OF J IN THE PARAMETER, not by the order of the difference. Six "
        "cases spanning the conditioning, seed %u, sampling material frozen (Task 3's detached "
        "instrument). Steps are check 39/40's MULTIPLIED (4x, or 2x where the roughness floor "
        "or the metallic clamp binds -- stated per case): that check picked h to make "
        "truncation SMALL, and this one measures it.\n",
        kGgxSeed);
    for (std::size_t i = 0; i < 6; ++i) {
        const Convergence3Fit& f = fits[i];
        const double tol = kConvergenceRelTol * std::fabs(f.analytic) * f.caseScale;
        std::printf(
            "    %-52s D(h) %.9g  D(2h) %.9g  D(4h) %.9g\n"
            "        h = %.9g (%.0fx check 39/40's %.9g); h^2 term %.6g (%.4g x tolerance); "
            "h^4 term %.6g, i.e. %.3g of the h^2 term\n"
            "        g %.9g; gradient error after removing BOTH %.4g, which is %.3g of the "
            "%.4g allowed at caseScale %.3g. Check 39/40 gate the same case at %.4g. The "
            "two calls land on the same 2h to %.3g\n",
            cases[i].name, f.d[0], f.d[1], f.d[2], f.h[0], f.h[0] / cases[i].singleH,
            static_cast<double>(cases[i].singleH), f.truncH2, std::fabs(f.truncH2) / tol,
            f.truncH4, std::fabs(f.truncH4 / f.truncH2), f.analytic, std::fabs(f.deltaImplied),
            std::fabs(f.deltaImplied) / tol, tol, f.caseScale, f.roundoffBound,
            std::fabs(f.crossCheck));
    }
    std::printf(
        "  Worst gradient error %.4g of tolerance. THE NULL TEST, which roughness and metallic "
        "were the one parameter family to lack: after the last case, all %u floats of the arena "
        "outside the single gradient element are EXACTLY 0.0f -- compared as floats, not "
        "through a tolerance. That covers a second registered parameter the scene does not "
        "depend on (both its blocks), this parameter's own Adam m/v state, and the 256-byte "
        "alignment padding where a mis-computed element index is likeliest to land.\n",
        worstMargin, kArenaFloats - 1u);
    return true;
}

// ===========================================================================
// 48. THE EMISSION GATE -- exactly linear, so truncation is identically zero
// ===========================================================================
bool checkEmissionConvergence(ohao::diff::GpuProbeContext& ctx) {
    constexpr float kMatAlbedo = 0.4f;
    constexpr float kEmission = 0.6f;
    constexpr float kStep = 0.0078125f;  // 2^-7, check 42's step
    constexpr uint32_t kGradientSeed = 20260829u;
    constexpr uint32_t kBounceCounts[3] = {1u, 2u, 3u};

    ohao::diff::ParamRegistry gradReg;
    const auto reg = gradReg.registerScalarBlock("emission", 1);
    if (!reg.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 48 registry setup: %s\n",
                     reg.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* emissionParam = gradReg.find("emission");
    if (emissionParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 48 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 48 gradient arena build\n");
        return false;
    }
    const uint32_t kArenaFloats =
        static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
    const uint32_t kOffset = static_cast<uint32_t>(
        gradReg.layout().block(emissionParam->gradBlock).offsetBytes / sizeof(float));

    GateScene scene;
    if (!scene.build(ctx, "check 48")) {
        gradArena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};
    ConvergenceFit fits[3]{};
    char names[3][48]{};
    bool ok = true;
    double worstMargin = 0.0;

    for (std::size_t i = 0; i < 3; ++i) {
        const uint32_t bounces = kBounceCounts[i];
        CrnFdMeasurement m1{};
        CrnFdMeasurement m2{};
        const bool okH = measureCrnEmissionGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, bounces, scene.camera, scene.positions,
            scene.indices, kMatAlbedo, kMaterial, kEmission, kStep, kGradientSeed, gradArena,
            emissionParam->gradBlock, kArenaFloats, kOffset, GateScene::kFilmRelativeEps, m1);
        const bool ok2H = measureCrnEmissionGradient(
            ctx, scene.wf, GateScene::kW, GateScene::kH, bounces, scene.camera, scene.positions,
            scene.indices, kMatAlbedo, kMaterial, kEmission, 2.0f * kStep, kGradientSeed,
            gradArena, emissionParam->gradBlock, kArenaFloats, kOffset,
            GateScene::kFilmRelativeEps, m2);
        if (!okH || !ok2H) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 48 measurement failed at %u bounce(s)\n",
                         bounces);
            scene.destroy(ctx);
            gradArena.destroy(ctx.allocator());
            return false;
        }
        fits[i] = fitConvergence(m1.finiteDiff, m1.hActual, m1.roundoffBound, m2.finiteDiff,
                                 m2.hActual, m2.roundoffBound, m1.analytic);
        std::snprintf(names[i], sizeof(names[i]), "%u bounce(s), J linear in emission", bounces);
        if (!gateFit("check 48", names[i], fits[i], /*truncationExpected=*/false, worstMargin)) {
            ok = false;
        }
    }

    scene.destroy(ctx);
    gradArena.destroy(ctx.allocator());
    if (!ok) return false;

    std::printf(
        "[diff_gpu_probe] OK: check 48 -- THE EMISSION GATE AT TWO STEP SIZES, and here "
        "asserting an h^2 FALLOFF WOULD BE ASSERTING SOMETHING FALSE. J is EXACTLY LINEAR in "
        "the emission (check 42's header: neither the throughput recursion nor the "
        "MIS-combined direct term ever reads pc.emission), so a central difference is exact "
        "and the truncation is identically zero at every h. The law asserted is that stronger "
        "one -- D(2h) and D(h) are the same number to the floor, at all three bounce counts -- "
        "and it is the check that the common-random-number precondition still holds. A "
        "parameter that leaked into a sampling decision, a density or a throughput would stop "
        "being linear and this would catch it ON J, where the existing trace-mismatch count "
        "catches it only on the path geometry. Seed %u, h = 2^-7 = %.9g and 2h, tolerance "
        "%.3g x |g|.\n",
        kGradientSeed, static_cast<double>(kStep), kConvergenceRelTol);
    for (std::size_t i = 0; i < 3; ++i) reportFit(names[i], fits[i], false);
    std::printf("  Worst gradient error %.4g of tolerance.\n", worstMargin);
    return true;
}

// ===========================================================================
// 49. THE EMISSION-TEXTURE GATE -- the same linearity, through a bilinear read
// ===========================================================================
bool checkTextureConvergence(ohao::diff::GpuProbeContext& ctx) {
    constexpr float kMatAlbedo = 0.4f;
    constexpr uint32_t kGradientSeed = 20260901u;
    constexpr uint32_t kBounces = 3u;
    constexpr float kUvScale = 0.0625f;  // 1/16: the floor's |x|,|z| <= 8 onto [0,1]
    constexpr float kUvBias = 0.5f;
    constexpr float kStep = 0.0078125f;  // 2^-7, check 45's step

    const ohao::diff::ParamShape kTexShape{4u, 3u, 3u};
    const uint32_t kTexFloats = kTexShape.floatCount();
    std::vector<float> baseTexels(kTexFloats, 0.0f);
    for (uint32_t y = 0; y < kTexShape.height; ++y) {
        for (uint32_t x = 0; x < kTexShape.width; ++x) {
            for (uint32_t c = 0; c < kTexShape.channels; ++c) {
                baseTexels[kTexShape.elementIndex(x, y, c)] =
                    0.30f + 0.05f * static_cast<float>(y * kTexShape.width + x) +
                    0.02f * static_cast<float>(c);
            }
        }
    }

    ohao::diff::ParamRegistry gradReg;
    const auto regTex =
        gradReg.registerTexture("emission_tex", kTexShape, VK_FORMAT_R32G32B32_SFLOAT);
    if (!regTex.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 49 registry setup: %s\n",
                     regTex.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* texParam = gradReg.find("emission_tex");
    if (texParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 49 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 49 gradient arena build\n");
        return false;
    }
    const uint32_t kArenaFloats =
        static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
    const uint32_t kOffset = static_cast<uint32_t>(
        gradReg.layout().block(texParam->gradBlock).offsetBytes / sizeof(float));

    GateScene scene;
    if (!scene.build(ctx, "check 49")) {
        gradArena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kTexMaterial{1.0f, 0.0f, 0.0f};

    // WHICH ELEMENT. The most-loaded one, read off a centre run. The
    // finite difference's floor is set by the WHOLE film's scale while the
    // quantity compared is ONE element's share of the gradient, so a
    // lightly-touched element cannot be resolved at any h -- check 45 makes
    // the same selection for the same reason, and states it at length. This
    // gate needs one element, not three: what it tests is the LINEARITY of J
    // in a texel, which is a property of the parameter and not of which texel
    // was picked.
    uint32_t element = 0;
    {
        std::vector<float> filmC;
        const ohao::diff::WavefrontGradientOptions options = emissionTextureOptions(
            baseTexels, kTexShape, kUvScale, kUvScale, kUvBias, kUvBias);
        if (!ctx.runWavefrontGradientProbe(
                scene.wf, GateScene::kW, GateScene::kH, kBounces, scene.camera,
                std::span<const float>(scene.positions), std::span<const uint32_t>(scene.indices),
                kMatAlbedo, kTexMaterial, kGradientSeed, gradArena, kArenaFloats, kOffset, filmC,
                options)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 49 centre run dispatch\n");
            scene.destroy(ctx);
            gradArena.destroy(ctx.allocator());
            return false;
        }
        const std::vector<float> block = gradArena.readback(ctx.allocator(), texParam->gradBlock);
        if (block.size() != static_cast<std::size_t>(kTexFloats)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 49 -- the texture's gradient block read "
                         "back %zu floats, expected %u\n",
                         block.size(), kTexFloats);
            scene.destroy(ctx);
            gradArena.destroy(ctx.allocator());
            return false;
        }
        element = static_cast<uint32_t>(
            std::max_element(block.begin(), block.end()) - block.begin());
    }

    CrnFdMeasurement m1{};
    CrnFdMeasurement m2{};
    const bool okH = measureCrnEmissionTexelGradient(
        ctx, scene.wf, GateScene::kW, GateScene::kH, kBounces, scene.camera, scene.positions,
        scene.indices, kMatAlbedo, kTexMaterial, baseTexels, kTexShape, kUvScale, kUvScale,
        kUvBias, kUvBias, element, kStep, kGradientSeed, gradArena, texParam->gradBlock,
        kArenaFloats, kOffset, GateScene::kFilmRelativeEps, m1);
    const bool ok2H = measureCrnEmissionTexelGradient(
        ctx, scene.wf, GateScene::kW, GateScene::kH, kBounces, scene.camera, scene.positions,
        scene.indices, kMatAlbedo, kTexMaterial, baseTexels, kTexShape, kUvScale, kUvScale,
        kUvBias, kUvBias, element, 2.0f * kStep, kGradientSeed, gradArena, texParam->gradBlock,
        kArenaFloats, kOffset, GateScene::kFilmRelativeEps, m2);
    scene.destroy(ctx);
    gradArena.destroy(ctx.allocator());
    if (!okH || !ok2H) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 49 measurement failed on element %u\n",
                     element);
        return false;
    }

    const ConvergenceFit fit =
        fitConvergence(m1.finiteDiff, m1.hActual, m1.roundoffBound, m2.finiteDiff, m2.hActual,
                       m2.roundoffBound, m1.analytic);
    char name[80]{};
    std::snprintf(name, sizeof(name), "texel element %u of %u, J linear in it", element,
                  kTexFloats);
    double worstMargin = 0.0;
    if (!gateFit("check 49", name, fit, /*truncationExpected=*/false, worstMargin)) return false;

    std::printf(
        "[diff_gpu_probe] OK: check 49 -- THE EMISSION-TEXTURE GATE AT TWO STEP SIZES: the same "
        "exact linearity as check 48, now reached THROUGH A BILINEAR RECONSTRUCTION. J is "
        "SUM_i w_i * texel_i in the forward read and the weights do not depend on the texel "
        "VALUES, so J is exactly linear in any one element and the truncation is identically "
        "zero -- which makes D(2h) - D(h) a direct test that the reconstruction has not made "
        "the read depend on what it read. A bilinear weight that picked up a texel value, or a "
        "footprint that moved with the perturbation, would show up here as curvature that the "
        "derivation says cannot exist. One element (the most-loaded, so the film-scale floor "
        "can resolve it -- check 45's selection, for check 45's stated reason), 4x3x3 texture, "
        "%u bounces, seed %u, h = 2^-7 = %.9g and 2h, tolerance %.3g x |g|.\n",
        kBounces, kGradientSeed, static_cast<double>(kStep), kConvergenceRelTol);
    reportFit(name, fit, false);
    std::printf("  Gradient error %.4g of tolerance.\n", worstMargin);
    return true;
}

}  // namespace ohao::diff::probe
