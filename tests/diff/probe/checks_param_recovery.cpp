// Stage 3, check 62: RECOVERY THROUGH THE PARAMETERISATION.
#include "probe/checks_param_recovery.hpp"

#include "diff/geom/vertex_parameterisation.hpp"
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
//
// The base shape is NOT a parameter. It is fixed for the life of the
// optimisation, and the three parameters act on it; that is the whole point
// of the indirection.
const std::vector<float>& paramBase() {
    static const std::vector<float> kBase = {2.0f, 1.5f, 2.4f, 5.6f, 5.9f, 3.4f};
    return kBase;
}

constexpr std::uint32_t kIterations = 200u;
constexpr float kAlpha = 0.03f;
// 3 * alpha, for Adam's terminal oscillation -- the same reasoning check 60
// uses, and the same multiple.
constexpr double kRecoveredWithin = 3.0 * static_cast<double>(kAlpha);
// The control must miss by a MARGIN, not marginally: a run that lands just
// outside the tolerance would leave open that the two runs differ by noise.
constexpr double kControlMissesBy = 10.0 * kRecoveredWithin;

struct Run {
    std::vector<float> theta;
    double firstLoss = 0.0;
    double lastLoss = 0.0;
    bool ok = false;
};

/// One optimisation, `flipScaleSign` selecting the control.
///
/// The loop is: apply the parameterisation, render coverage, L2 loss, the
/// GPU boundary pass seeded with dL/dpixel, THE PULLBACK, one Adam step.
/// The boundary pass never learns that a parameterisation exists -- it is
/// handed positions and returns dL/d(position), exactly as before. The
/// parameterisation sits entirely between it and the optimiser, which is
/// what makes a different one (Laplacian, iso-surface) a substitution
/// rather than a rewrite.
Run runRecovery(ohao::diff::GpuProbeContext& ctx,
                const ohao::diff::AffineVertexParameterisation& param,
                const std::vector<float>& target, const std::vector<float>& theta0,
                bool flipScaleSign, const char* label) {
    Run run;
    run.theta = theta0;
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};
    const std::size_t pixels = target.size();

    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> adamState(run.theta.size() * 2u, 0.0f);

    for (std::uint32_t it = 1; it <= kIterations; ++it) {
        const std::vector<float> positions = param.apply(run.theta);
        if (positions.size() != 6u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 62 (%s) -- apply returned %zu floats at "
                         "iteration %u\n",
                         label, positions.size(), it);
            return run;
        }
        const std::vector<float> image =
            renderTriangleCoverage(positions, kImage, kSub, kLTri, kLEnv);

        // L = (1/N) SUM (I - T)^2, so dL/dI_p = 2 (I_p - T_p) / N. On the
        // host, as in check 60: loss_l2.comp is written for a 3-channel
        // film and this is a 1-channel coverage image. The formula is
        // check 51's, and check 51 is what gates it.
        double loss = 0.0;
        std::vector<float> seed(pixels, 0.0f);
        for (std::size_t p = 0; p < pixels; ++p) {
            const double d = static_cast<double>(image[p]) - static_cast<double>(target[p]);
            loss += d * d / static_cast<double>(pixels);
            seed[p] = static_cast<float>(2.0 * d / static_cast<double>(pixels));
        }
        if (it == 1u) run.firstLoss = loss;
        run.lastLoss = loss;

        std::vector<float> posGrad;
        if (!ctx.runBoundaryProbe(positions, edges, kImage, kImage,
                                  {static_cast<float>(kLTri), static_cast<float>(kLEnv)}, {}, seed, nullptr, 0u, 0u, posGrad)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 62 (%s) boundary dispatch at iter %u\n",
                         label, it);
            return run;
        }

        // THE PULLBACK. Six position gradients become three parameter
        // gradients; this is the only place the parameterisation enters the
        // gradient path.
        std::vector<float> thetaGrad = param.pullback(run.theta, posGrad);
        if (thetaGrad.size() != ohao::diff::AffineVertexParameterisation::kParamCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 62 (%s) -- pullback returned %zu floats at "
                         "iteration %u\n",
                         label, thetaGrad.size(), it);
            return run;
        }
        if (flipScaleSign) thetaGrad[2] = -thetaGrad[2];

        if (!ctx.runAdamProbe(run.theta, thetaGrad, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 62 (%s) Adam step at iter %u\n",
                         label, it);
            return run;
        }
    }
    run.ok = true;
    return run;
}

double worstError(const std::vector<float>& a, const std::vector<float>& b, std::size_t& worstIdx) {
    double worst = 0.0;
    worstIdx = 0;
    for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
        const double err = std::fabs(static_cast<double>(a[k]) - static_cast<double>(b[k]));
        if (err > worst) {
            worst = err;
            worstIdx = k;
        }
    }
    return worst;
}

}  // namespace

bool checkParameterisedRecovery(ohao::diff::GpuProbeContext& ctx) {
    const std::vector<float> star = {0.35f, -0.25f, 0.05f};   // tx, ty, log-scale
    const std::vector<float> theta0 = {-0.5f, 0.6f, -0.22f};

    ohao::diff::AffineVertexParameterisation param;
    if (!param.setBase(paramBase())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 62 -- setBase rejected the base\n");
        return false;
    }

    const std::vector<float> starPositions = param.apply(star);
    const std::vector<float> startPositions = param.apply(theta0);
    const std::vector<float> target =
        renderTriangleCoverage(starPositions, kImage, kSub, kLTri, kLEnv);

    // --- NON-VACUITY OF THE STARTING POINT, before anything is optimised.
    //
    // The log-scale must be genuinely WRONG at theta_0, not merely offset in
    // translation. Translation alone drives the two Jacobian columns that are
    // constant; only the scale column depends on the base shape, so a start
    // that differed only in tx and ty would exercise the trivial half of the
    // pullback and pass whatever the third column contained.
    const double scaleError = std::fabs(static_cast<double>(theta0[2] - star[2]));
    if (!(scaleError > 3.0 * kRecoveredWithin)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 62 -- the log-scale starts %.6g from theta*, "
                     "inside 3x the recovery tolerance %.6g. The scale column is the only part of "
                     "the Jacobian that is not constant, so a run that does not have to move it "
                     "tests the trivial half of the pullback\n",
                     scaleError, kRecoveredWithin);
        return false;
    }

    const Run primary = runRecovery(ctx, param, target, theta0, false, "main");
    if (!primary.ok) return false;

    std::size_t worstIdx = 0;
    const double worst = worstError(primary.theta, star, worstIdx);
    std::size_t startIdx = 0;
    const double startTheta = worstError(theta0, star, startIdx);
    static const char* const kNames[3] = {"tx", "ty", "logScale"};

    if (!(primary.firstLoss > 0.0) || !std::isfinite(primary.firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 62 -- the loss at theta_0 is %.9g. It must be "
                     "finite and strictly positive, or the optimiser started at the answer\n",
                     primary.firstLoss);
        return false;
    }
    if (!(worst <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 62 -- RECOVERY THROUGH THE PARAMETERISATION: the three "
            "parameters did not recover.\n"
            "  worst parameter %s: theta* = %.9g, theta_0 = %.9g, theta = %.9g\n"
            "  |error| = %.6g, above the PRE-REGISTERED %.6g (3 * alpha)\n"
            "  loss %.9g -> %.9g over %u iterations\n"
            "  ATTRIBUTION. Check 60 runs this same loop on the six positions directly and is\n"
            "  green, so the boundary pass, the seed and the Adam step are not at fault; the\n"
            "  ONE thing added here is the pullback between them. The interior term is still\n"
            "  exactly zero (constant radiance on each side), so a failure is not the interior\n"
            "  machinery either.\n",
            kNames[worstIdx], static_cast<double>(star[worstIdx]),
            static_cast<double>(theta0[worstIdx]), static_cast<double>(primary.theta[worstIdx]),
            worst, kRecoveredWithin, primary.firstLoss, primary.lastLoss, kIterations);
        return false;
    }
    if (!(primary.lastLoss < primary.firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 62 -- the parameters arrived but the loss went "
                     "%.9g -> %.9g, which did not fall. That would mean the two are not measuring "
                     "the same image\n",
                     primary.firstLoss, primary.lastLoss);
        return false;
    }

    // --- SIX COMPONENTS FROM THREE KNOBS. The parameters landing on theta*
    // and the SHAPE landing on the target shape are not the same statement
    // unless the Jacobian is the one `apply` implies -- three numbers cannot
    // place six independently.
    const std::vector<float> finalPositions = param.apply(primary.theta);
    std::size_t worstPosIdx = 0;
    const double worstPos = worstError(finalPositions, starPositions, worstPosIdx);
    std::size_t startPosIdx = 0;
    const double startPos = worstError(startPositions, starPositions, startPosIdx);
    // exp(s) magnifies a parameter error into a position error by roughly the
    // largest base coordinate, so the position tolerance is scaled by it.
    double baseSpan = 0.0;
    for (float b : paramBase()) baseSpan = std::max(baseSpan, std::fabs(static_cast<double>(b)));
    const double kPositionWithin = kRecoveredWithin * (1.0 + baseSpan);
    if (!(worstPos <= kPositionWithin)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 62 -- the parameters recovered to within %.6g "
                     "but the SHAPE they produce is %.6g from the target at component %zu, above "
                     "%.6g. Three parameters cannot place six components unless the Jacobian is "
                     "the one `apply` implies\n",
                     worst, worstPos, worstPosIdx, kPositionWithin);
        return false;
    }

    // --- THE CONTROL. Everything identical but the sign of the scale
    // column, which is the bug class that actually happened twice in this
    // stage's boundary work (a reversed edge normal, and a closed form
    // carrying the wrong sign). It must NOT recover, and must miss by a
    // margin -- otherwise "the main run recovered" says nothing about
    // whether the pullback steered it.
    const Run control = runRecovery(ctx, param, target, theta0, true, "control");
    if (!control.ok) return false;
    std::size_t controlIdx = 0;
    const double controlWorst = worstError(control.theta, star, controlIdx);
    if (!(controlWorst > kControlMissesBy)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 62 -- THE CONTROL RECOVERED. With the sign of the "
            "scale column of the pullback flipped, the parameters still landed %.6g from "
            "theta* (worst: %s), inside the pre-registered margin %.6g. That means the main "
            "run's success does not depend on the scale column being right, and this gate is "
            "measuring the translation columns and the optimiser -- both of which check 60 "
            "already covers\n",
            controlWorst, kNames[controlIdx], kControlMissesBy);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 62 -- RECOVERY THROUGH THE PARAMETERISATION. Three "
        "parameters (tx, ty, log-scale) recovered from a synthetic target by descending the "
        "boundary term, with dL/d(position) reaching them ONLY through the pullback. Worst "
        "parameter finished %.6g from theta* against a PRE-REGISTERED %.6g (3 * alpha), having "
        "started %.4g away; loss %.9g -> %.9g over %u Adam iterations at alpha = %.4g. This is "
        "what spec 9's \"never vertex positions directly\" asks for, and check 60 -- which "
        "optimises the six positions themselves -- is precisely the case it rules out. The six "
        "position components follow: the shape lands %.6g from the target shape (started %.4g "
        "away), which three numbers cannot do unless the Jacobian is the one `apply` implies. "
        "THE LOG-SCALE STARTS %.4g FROM THE ANSWER, so the run cannot pass on the two constant "
        "Jacobian columns alone. WHAT THIS GATE CANNOT SEE, said plainly: the target is exactly "
        "reachable, so at the optimum every position gradient vanishes and so does every "
        "pullback of it -- right or wrong, the optimum is a common fixed point -- and Adam "
        "divides by sqrt(v), so a positive rescaling of any component changes nothing at all. A "
        "magnitude error in J is therefore invisible HERE and is gated instead by the unit test "
        "that builds J column by column from finite differences of `apply`. What is gated here "
        "is the structure, and the CONTROL is what makes that claim non-vacuous: the identical "
        "run with the scale column's sign flipped finishes %.4g from theta* (worst: %s), past "
        "the pre-registered margin of %.4g -- the bug class that actually occurred twice in "
        "this stage's boundary work.\n",
        worst, kRecoveredWithin, worstError(theta0, star, worstIdx), primary.firstLoss,
        primary.lastLoss, kIterations, static_cast<double>(kAlpha), worstPos, startPos, scaleError,
        controlWorst, kNames[controlIdx], kControlMissesBy);
    return true;
}

}  // namespace ohao::diff::probe
