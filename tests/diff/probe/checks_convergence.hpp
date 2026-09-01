// Stage 1 Task 6: THE FOUR GATES, each asserted at TWO STEP SIZES.
//
// Checks 37-45 each measure a gradient at ONE step size and accept it when
// |D(h) - g| falls under a derived bound. That bound is sound but loose, and
// -- more to the point -- a single-h agreement cannot distinguish a correct
// gradient from one that is wrong by less than the bound. A CONVERGENCE RATE
// can: the finite difference carries a truncation error whose SIZE is set by
// h and whose EXISTENCE is set by the analytic form of J, so measuring at two
// step sizes and asking whether the two errors stand in the ratio the theory
// predicts tests the gradient against a law rather than against a tolerance.
//
// WHAT LAW, THOUGH -- and this is where the plan's wording ("confirm the FD
// error falls as h^2") is right for one of the four gates and WRONG for two
// of them. The order of a central difference is a property of the DIFFERENCE;
// the order of its error is a property of the FUNCTION. J is a different kind
// of function of each of these four parameters:
//
//   * ALBEDO. J(a) = SUM_{n=1..B} K_n a^n exactly, for a pure Lambertian
//     surface at B bounces (fd_harness.cpp derives this). A central
//     difference is EXACT on n = 1 and n = 2, so at B = 1 and B = 2 the
//     truncation error is IDENTICALLY ZERO -- there is no h^2 to observe --
//     and at B = 3 it is EXACTLY K_3 * h^2 with NO higher-order remainder,
//     because the polynomial stops at n = 3. So the albedo gate carries both
//     laws at once, and which one applies is decided by the bounce count.
//   * ROUGHNESS / METALLIC. J is smooth but not polynomial, so the truncation
//     is C*h^2 + O(h^4) -- the generic case, and the only one of the four
//     where "falls as h^2" is an asymptotic statement rather than an exact
//     one.
//   * EMISSION and EMISSION TEXTURE. J is EXACTLY LINEAR in either (check
//     42's header states why: neither the throughput recursion nor the
//     MIS-combined direct term ever reads the emission). Truncation is
//     identically zero at every h, so asserting an h^2 falloff here would be
//     asserting something FALSE. The law to assert is the stronger one:
//     D(2h) - D(h) is zero to roundoff.
//
// That the last two are exactly linear is not a weaker check than an h^2
// falloff -- it is a sharper one, and it is the check that the CRN
// precondition still holds. A parameter that leaked into a sampling decision,
// a throughput, or a density would stop being linear, and D(2h) - D(h) would
// pick that up directly on J, where the existing trace-mismatch count only
// picks it up on the path geometry.
//
// WHY A SECOND STEP SIZE BUYS DETECTION AND NOT JUST CONFIRMATION. Write the
// analytic gradient as g = g_true + delta. Then
//
//     e1 := D(h) - g            = K*h^2 - delta
//     T  := (D(2h) - D(h)) / 3  = K*h^2          <- no g in it at all
//     T - e1                    = delta          <- EXACTLY, for any K
//
// so the pair isolates the gradient's error from the truncation term
// completely, at every bounce count and whether or not K is zero. That
// difference is what these checks gate on. It is not bounded by the
// roundoff BOUND the single-h checks carry -- which overstates the true
// roundoff by ~10^3, because it does not model the cancellation common
// random numbers produce between J(a+h) and J(a-h) -- but by the actual
// measurement floor, which the B = 1 and B = 2 albedo cases measure directly
// (K is exactly zero there, so whatever they show IS the floor).
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

/// The two-step-size fit shared by all four gates: given D(h1) and D(h2)
/// against one analytic gradient, recover the truncation coefficient the pair
/// implies, the Richardson extrapolant that removes it, and the ratio the two
/// errors actually stand in.
struct ConvergenceFit {
    double h1{0.0}, h2{0.0};
    double d1{0.0}, d2{0.0};
    double analytic{0.0};
    /// SIGNED errors. The sign is the whole point: |e1| against |T| can pass
    /// with the two pointing opposite ways, which is exactly the discrepancy
    /// a wrong gradient produces.
    double e1{0.0}, e2{0.0};
    /// (D(h2) - D(h1)) * h1^2 / (h2^2 - h1^2) == K*h1^2 under the h^2 model.
    /// Built from the two MEASURED differences alone -- the analytic gradient
    /// does not enter it, which is what lets it be compared against e1.
    double truncMeasured{0.0};
    /// (h2^2*D1 - h1^2*D2) / (h2^2 - h1^2): the h^2 term removed.
    double richardson{0.0};
    double richardsonResidual{0.0};
    double roundoff1{0.0}, roundoff2{0.0}, richardsonRoundoff{0.0};
    /// e2/e1 against (h2/h1)^2. Only meaningful when truncation dominates the
    /// floor; the gate quotes it only for the cases where it does.
    double observedRatio{0.0};
    double expectedRatio{0.0};
    /// truncMeasured - e1. Under a correct gradient both are K*h1^2 and this
    /// is zero to the measurement floor; under a gradient wrong by delta this
    /// IS delta, undiluted by the loose roundoff bound.
    double deltaImplied{0.0};
};

ConvergenceFit fitConvergence(double d1, double h1, double roundoff1, double d2, double h2,
                              double roundoff2, double analytic);

/// HOW MANY STEP SIZES A GATE NEEDS IS SET BY THE POLYNOMIAL DEGREE OF J,
/// and this is the one place the plan's "two step sizes" is not enough.
///
/// Expand the central difference: D(h) = g + C*h^2 + E*h^4 + ...  A TWO-point
/// fit solves for g and C and therefore reports
///
///     deltaImplied = delta + 4*E*h^4
///
/// -- the gradient's error CONTAMINATED by the first term the model left out.
/// Where J is a polynomial of degree <= 3 in the parameter that costs nothing,
/// because E is then exactly zero: the albedo (cubic at 3 bounces), the
/// emission and the emission texture (both exactly linear) are all in that
/// case, and two points are provably sufficient for them.
///
/// ROUGHNESS AND METALLIC ARE NOT. J is smooth but not polynomial in either,
/// E is genuinely nonzero, and a two-point fit charges 4*E*h^4 to the
/// gradient. Measured, that term reaches 5x the gradient's actual error -- so
/// a two-point gate on these two would reject a CORRECT adjoint and blame the
/// wrong thing. Three points remove it: fit g, C and E through h, 2h and 4h,
/// leaving O(h^6).
struct Convergence3Fit {
    double h[3]{};
    double d[3]{};
    double analytic{0.0};
    /// g from the quadratic through (h^2, D) at h^2 = 0.
    double extrapolated{0.0};
    /// The fitted C*h1^2 and E*h1^4 -- reported so that "the h^4 term was
    /// worth removing" is a measurement rather than an assertion.
    double truncH2{0.0};
    double truncH4{0.0};
    /// extrapolated - analytic: the gradient's error with BOTH modelled
    /// truncation terms removed.
    double deltaImplied{0.0};
    /// The two calls both measure D(2h), and this is their difference. It
    /// comes out EXACTLY zero, and that is the honest reading of it: the
    /// forward film is deterministic (check 36), so two renders at the same
    /// parameter value and seed are bit-identical and so are the central
    /// differences built from them. What this therefore verifies is that the
    /// two calls really did land on the same step -- a ladder that had
    /// silently skewed would show here. It is NOT a measurement of the
    /// arena's floor, and an earlier version of this comment claimed it was:
    /// the arena's float-atomicAdd non-determinism lives on the ANALYTIC
    /// side, which this difference does not touch.
    double crossCheck{0.0};
    /// The harness's roundoff bound at h1. REPORTED ONLY -- it is what the
    /// single-h checks gate on, quoted so the two resolutions are comparable
    /// in one line. It is not part of this gate's tolerance; see the header
    /// for why it was tried there and rejected.
    double roundoffBound{0.0};
    /// This case's conditioning factor, (0.60/r)^2 for roughness and 1
    /// otherwise. Carried on the fit so the report can print it.
    double caseScale{1.0};
};

Convergence3Fit fitConvergence3(const double* h, const double* d, double analytic);

/// THE TOLERANCE: one pre-registered relative number, times a per-case
/// conditioning factor that is 1 everywhere except the GGX roughness cases.
///
///     tol = kConvergenceRelTol * |g| * caseScale
///
/// THE MEASUREMENT FLOOR IS NOT THE SAME FOR EVERY PARAMETER, and pretending
/// it is would either make the gate flaky on the sharp lobes or useless on
/// the quiet ones. Measured, as a fraction of |g|, the residual this gate
/// reads floats at:
///
///     emission texture        2.4e-7      albedo (3 bounces)  9.2e-7
///     metallic (both cases)   1.7e-6 .. 3.9e-6
///     roughness r=0.60        2.4e-6      r=0.35   9.2e-6
///     roughness r=0.10        1.4e-5      r=0.04   1.9e-4
///
/// Everything except roughness sits comfortably under 1e-5. The roughness
/// cases climb monotonically as the lobe narrows, across nearly two orders of
/// magnitude, and that is not noise in the reading: sweeping the near-specular
/// case over three seeds gives -0.040, -0.065, -0.081 -- same sign, spread
/// comparable to the value -- and over four step sizes gives +0.18, +0.27,
/// -0.054, -0.040, which neither grows as h^2 nor falls as 1/h.
///
/// THE MECHANISM IS THAT COMMON RANDOM NUMBERS STOP CANCELLING. The floor for
/// a broad lobe rests on J(theta+h) and J(theta-h) being nearly equal numbers
/// whose rounding errors are strongly correlated. Perturb the roughness of a
/// lobe of width alpha = r^2 and the reshaping is what the difference is made
/// of; the narrower the lobe, the more each film keeps of its own rounding.
/// So the floor scales with 1/alpha = 1/r^2, and the GGX case table carries
/// that ONE law rather than four separately-chosen numbers -- normalised at
/// the broadest case, caseScale = (0.60/r)^2 for roughness and 1 for
/// metallic, which is a law that predicts the trend rather than a fit to it.
/// It over-predicts at r = 0.10 (36 against a needed ~6) and slightly
/// under-predicts the seed spread at r = 0.04, so it is conservative where it
/// is wrong in the direction that matters.
///
/// AN EARLIER VERSION USED A FRACTION OF THE HARNESS'S `roundoffBound`
/// INSTEAD, and it was a bad model: that bound overestimates the texture
/// case's true floor by ~1800x and the near-specular case's by ~13x, so it
/// loosened every quiet gate to buy safety for one sharp one. Under it check
/// 49 resolved at 4.3e-4 -- WORSE than check 44's conservation identity,
/// which makes 49 redundant for the texture rather than additive. Named here
/// because the replacement looks like a complication and is in fact the
/// simpler claim.
///
/// THE RELATIVE TERM, DERIVED. `deltaImplied` must fall under this fraction
/// of |g|, times the case's scale.
///
/// The analytic side is a float32 atomicAdd of capacity*bounces contributions
/// into one accumulator; with rounding in arbitrary order that is a
/// sqrt(N)*ulp(|g|) walk, which at this probe's largest case (N = 512*3,
/// |g| ~ 2.2e3, ulp = 2^-12) is 4.4e-6 relative. The finite-difference side
/// contributes a residual of the same order, measured rather than modelled:
/// the B = 1 and B = 2 albedo cases have K identically zero, so their
/// deltaImplied is pure floor, and it comes out at 3-5e-7. 1e-5 is the round
/// number above the sum, and every gate reports its observed margin so a
/// reader can see the gates run at a few percent of it rather than against
/// the edge.
inline constexpr double kConvergenceRelTol = 1e-5;

/// For a case whose truncation is meant to be PRESENT, |truncH2| must exceed
/// the tolerance by this factor. Without it, "the error falls as h^2" could
/// be confirmed on an h^2 term indistinguishable from zero -- the vacuity
/// this probe keeps finding in checks that read rigorous.
///
/// THE VALUE IS NOT A FEEL. It answers exactly one question: WOULD REMOVING
/// THE TRUNCATION HAVE CHANGED THE VERDICT? A gate that did not remove it
/// compares |D(h) - g|, which carries the truncation whole; so if
/// |truncH2| >= tol the un-removed error exceeds what the gate allows, and at
/// 3x it exceeds it by a margin no measurement floor could explain away. Any
/// case clearing this factor is one where the extrapolation is load-bearing
/// rather than decorative -- below it, the check would be a single-h gate
/// wearing a convergence gate's report.
inline constexpr double kTruncationPresentFactor = 3.0;

/// Check 46: the albedo gate. Carries both laws -- truncation identically
/// zero at 1 and 2 bounces, exactly K*h^2 at 3.
bool checkAlbedoConvergence(ohao::diff::GpuProbeContext& ctx);

/// Check 47: the roughness/metallic gate, the only genuinely asymptotic h^2.
bool checkGgxConvergence(ohao::diff::GpuProbeContext& ctx);

/// Check 48: the emission gate -- exactly linear, truncation identically zero.
bool checkEmissionConvergence(ohao::diff::GpuProbeContext& ctx);

/// Check 49: the emission-texture gate, the same linearity through a bilinear
/// reconstruction.
bool checkTextureConvergence(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
