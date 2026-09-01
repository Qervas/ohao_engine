// Stage 2 Task 3, check 52: Adam.
#include "probe/checks_adam.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

/// Kingma & Ba 2015, Algorithm 1, in double, written FROM THE PAPER and not
/// from optimizer_adam.comp. An oracle transcribed from the thing it checks
/// cannot fail; this one is transcribed from the source both are supposed to
/// implement, which is the same discipline oracle_bsdf.cpp follows for the
/// published BSDF formulae.
struct CpuAdam {
    double alpha, beta1, beta2, epsilon;
    std::vector<double> m, v;

    explicit CpuAdam(std::size_t n, double a, double b1, double b2, double eps)
        : alpha(a), beta1(b1), beta2(b2), epsilon(eps), m(n, 0.0), v(n, 0.0) {}

    /// t is 1-based, as in the paper.
    void step(std::vector<double>& theta, const std::vector<double>& g, std::uint32_t t) {
        const double bc1 = 1.0 - std::pow(beta1, static_cast<double>(t));  // line 7 denominator
        const double bc2 = 1.0 - std::pow(beta2, static_cast<double>(t));  // line 8 denominator
        for (std::size_t i = 0; i < theta.size(); ++i) {
            m[i] = beta1 * m[i] + (1.0 - beta1) * g[i];              // line 5
            v[i] = beta2 * v[i] + (1.0 - beta2) * g[i] * g[i];       // line 6
            const double mHat = m[i] / bc1;                          // line 7
            const double vHat = v[i] / bc2;                          // line 8
            theta[i] -= alpha * mHat / (std::sqrt(vHat) + epsilon);  // line 9
        }
    }
};

}  // namespace

bool checkAdam(ohao::diff::GpuProbeContext& ctx) {
    // Not a multiple of 64, so the dispatch tail is exercised.
    constexpr std::size_t kN = 11;
    constexpr std::uint32_t kSteps = 50;
    // float32 throughout the GPU side against a double reference, so the
    // agreement is limited by the narrower type. A relative floor rather than
    // absolute: Adam's step is scale-free in the gradient, so the parameter
    // values are what set the scale here.
    constexpr double kRelTol = 2e-5;

    ohao::diff::GpuProbeContext::AdamOptions options;  // the paper's defaults

    // THE OBJECTIVE: f(theta) = SUM_i (theta_i - c_i)^2, so g_i = 2(theta_i -
    // c_i) and the minimiser is c, exactly. A closed-form objective, so the
    // gradient below is not produced by anything this check is testing.
    std::vector<double> centre(kN, 0.0);
    std::vector<float> theta(kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i) {
        centre[i] = 0.25 + 0.1 * static_cast<double>(i % 4u);
        theta[i] = static_cast<float>(1.5 - 0.2 * static_cast<double>(i % 3u));
    }
    std::vector<double> thetaRef(theta.begin(), theta.end());
    std::vector<float> state(kN * 2u, 0.0f);
    CpuAdam ref(kN, options.alpha, options.beta1, options.beta2, options.epsilon);

    // --- ASSERTION 1: THE FIRST STEP HAS A CLOSED FORM, derived on paper.
    //
    // With m_0 = v_0 = 0, line 5 gives m_1 = (1-b1) g and line 7 divides by
    // (1-b1^1) = (1-b1), so mHat_1 = g EXACTLY. Likewise vHat_1 = g^2. Line 9
    // is then
    //
    //     theta_1 = theta_0 - alpha * g / (|g| + eps)
    //
    // which is alpha * sign(g) up to eps, for ANY gradient. This needs no
    // reference implementation, and it is where a missing bias correction is
    // largest: without lines 7-8 the same step would be
    // alpha * 0.1 |g| / (0.0316 |g|) = 3.16 * alpha, more than three times
    // too far.
    {
        std::vector<float> g1(kN, 0.0f);
        for (std::size_t i = 0; i < kN; ++i) {
            g1[i] = static_cast<float>(2.0 * (static_cast<double>(theta[i]) - centre[i]));
        }
        std::vector<float> theta1 = theta;
        std::vector<float> state1 = state;
        if (!ctx.runAdamProbe(theta1, g1, state1, options, 1u)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 52 first-step dispatch\n");
            return false;
        }
        for (std::size_t i = 0; i < kN; ++i) {
            const double g = static_cast<double>(g1[i]);
            const double expected = static_cast<double>(theta[i]) -
                                    options.alpha * g /
                                        (std::fabs(g) + static_cast<double>(options.epsilon));
            const double got = static_cast<double>(theta1[i]);
            if (!(std::fabs(got - expected) <= kRelTol * std::fabs(expected))) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 52 -- THE FIRST STEP IS NOT alpha * sign(g).\n"
                    "  element %zu: theta_0 = %.9g, g = %.9g\n"
                    "  theta_1 should be %.12g and is %.12g\n"
                    "  With m_0 = v_0 = 0 the bias correction makes mHat_1 = g and vHat_1 = g^2\n"
                    "  EXACTLY, so this is closed form for any gradient and needs no reference\n"
                    "  implementation. WITHOUT lines 7-8 the step would be 3.16x this one --\n"
                    "  which is what a ratio near 3.16 below means. An uncorrected Adam still\n"
                    "  CONVERGES on this objective, so the endpoint would not have told you.\n",
                    i, static_cast<double>(theta[i]), g, expected, got);
                return false;
            }
        }
    }

    // --- ASSERTION 2: THE WHOLE TRAJECTORY, against the paper's algorithm.
    double worstRel = 0.0;
    std::uint32_t worstStep = 0;
    for (std::uint32_t t = 1; t <= kSteps; ++t) {
        std::vector<float> g(kN, 0.0f);
        std::vector<double> gRef(kN, 0.0);
        for (std::size_t i = 0; i < kN; ++i) {
            g[i] = static_cast<float>(2.0 * (static_cast<double>(theta[i]) - centre[i]));
            gRef[i] = 2.0 * (thetaRef[i] - centre[i]);
        }
        if (!ctx.runAdamProbe(theta, g, state, options, t)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 52 dispatch at step %u\n", t);
            return false;
        }
        ref.step(thetaRef, gRef, t);
        for (std::size_t i = 0; i < kN; ++i) {
            const double got = static_cast<double>(theta[i]);
            const double want = thetaRef[i];
            const double rel = std::fabs(got - want) / std::fabs(want);
            if (rel > worstRel) {
                worstRel = rel;
                worstStep = t;
            }
            if (!(rel <= kRelTol)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 52 -- the trajectory diverges from "
                             "Kingma & Ba's algorithm at STEP %u, element %zu: %.12g against "
                             "%.12g, relative %.6g above %.3g.\n"
                             "  THE STEP NUMBER LOCALISES THE CAUSE. A divergence at the first "
                             "few steps and not later is the bias correction (lines 7-8), whose "
                             "factors are within a percent of 1 by t ~ 100. A divergence that "
                             "grows with t is the moment recursion (lines 5-6). One that is "
                             "there from t = 1 and constant is alpha or epsilon.\n",
                             t, i, got, want, rel, kRelTol);
                return false;
            }
        }
    }

    // --- ASSERTION 3: IT ACTUALLY MOVED, and towards the minimiser.
    //
    // Without this the two implementations could agree perfectly on a
    // trajectory that goes nowhere -- the vacuity this probe keeps finding.
    double startDist = 0.0;
    double endDist = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        const double d0 = (1.5 - 0.2 * static_cast<double>(i % 3u)) - centre[i];
        const double d1 = static_cast<double>(theta[i]) - centre[i];
        startDist += d0 * d0;
        endDist += d1 * d1;
    }
    startDist = std::sqrt(startDist);
    endDist = std::sqrt(endDist);
    // THE CRITERION IS ADAM'S OWN STEP BOUND, not a round fraction.
    //
    // Line 9's update is alpha * mHat / (sqrt(vHat) + eps), and mHat /
    // sqrt(vHat) is bounded by roughly 1 in magnitude -- that scale-freedom
    // is the point of the method -- so NO parameter can move more than about
    // alpha per step. Over kSteps steps and kN parameters the distance to the
    // minimiser therefore cannot fall by more than alpha * sqrt(kN) * kSteps.
    //
    // An earlier version of this assertion demanded the distance HALVE,
    // which at alpha = 1e-3 over 50 steps was arithmetically impossible: the
    // bound is 0.166 against a starting distance of 3.14. That was a gate
    // that could only fail, which is as useless as one that can only pass.
    //
    // What is asserted instead is that Adam achieved most of that bound --
    // i.e. it is taking near-maximal steps STRAIGHT at the minimiser, which
    // is exactly what it does far from the optimum, where mHat/sqrt(vHat)
    // saturates towards sign(g). That is a real property of the method and
    // a trajectory going nowhere fails it.
    const double kMaxClosure =
        static_cast<double>(options.alpha) * std::sqrt(static_cast<double>(kN)) *
        static_cast<double>(kSteps);
    const double closed = startDist - endDist;
    if (!(closed >= 0.9 * kMaxClosure)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 52 -- after %u steps the distance to the "
                     "minimiser went from %.9g to %.9g, closing %.9g. Adam's own step bound is "
                     "alpha * sqrt(N) * steps = %.9g, and far from the optimum it should reach "
                     "most of that -- mHat/sqrt(vHat) saturates towards sign(g) there, so the "
                     "step is essentially alpha straight at the minimiser. Closing much less "
                     "means the direction is wrong or the steps are not full; two "
                     "implementations agreeing on a trajectory that goes nowhere would satisfy "
                     "every assertion above this one\n",
                     kSteps, startDist, endDist, closed, kMaxClosure);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 52 -- ADAM (Kingma & Ba 2015, Algorithm 1), against a CPU "
        "reference written FROM THE PAPER rather than from the shader, over the WHOLE %u-step "
        "trajectory and not just its endpoint. Worst relative divergence %.3g at step %u, "
        "against %.3g. The step number is diagnostic: an early-only divergence is the bias "
        "correction, a growing one is the moment recursion, a constant one is alpha or epsilon. "
        "THE FIRST STEP IS ALSO CHECKED IN CLOSED FORM, derived on paper and needing no "
        "reference at all -- with m_0 = v_0 = 0 the correction makes mHat_1 = g and vHat_1 = g^2 "
        "exactly, so theta_1 = theta_0 - alpha*g/(|g|+eps), i.e. alpha*sign(g). That is where "
        "lines 7-8 matter most: without them the first step is 3.16x too far, and by t ~ 100 the "
        "difference has vanished entirely -- an uncorrected Adam still CONVERGES here, so the "
        "endpoint could never have caught it. NON-VACUITY: the distance to the minimiser fell "
        "%.9g -> %.9g over %zu parameters, closing %.4g of the %.4g that alpha * sqrt(N) * steps "
        "allows AT ALL -- Adam far from the optimum takes near-maximal steps straight at the "
        "minimum, so agreeing on a trajectory that went nowhere would not pass this.\n",
        kSteps, worstRel, worstStep, kRelTol, startDist, endDist, kN, closed, kMaxClosure);
    return true;
}

}  // namespace ohao::diff::probe
