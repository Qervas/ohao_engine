// The finite-difference harnesses (Stage 1 Tasks 2-5).
//
// Lifted verbatim out of diff_gpu_probe.cpp: the same code with the same
// derivations. The three measurement structs it used to declare at file
// scope now live in fd_harness.hpp -- a linkage change, not a value change.
#include "probe/fd_harness.hpp"

#include "diff/wavefront/wavefront_loop.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace ohao::diff::probe {

// ===========================================================================
// THE CRN FINITE-DIFFERENCE HARNESS (Stage 1 Task 2)
// ===========================================================================
//
// One reusable measurement of a scalar derivative of the rendered image, by
// central difference under COMMON RANDOM NUMBERS, against whatever the
// gradient arena holds.
//
// It is a helper and not three transcriptions inside three checks because
// Tasks 3 and 4 differentiate the OTHER scalar material parameters through
// exactly this shape -- perturb one pushed float, render at +h and -h with the
// identical (pixel, sampleIndex, iterationSeed), difference the films -- and
// three hand-written harnesses would be three chances at the same defect and
// three oracles to audit. Task 6 adds the two-step-size convergence on top of
// this, not beside it.
//
// ---------------------------------------------------------------------------
// WHAT IS DIFFERENTIATED, AND WHY COMMON RANDOM NUMBERS MAKE THIS SHARP
// ---------------------------------------------------------------------------
//
// The scalar is
//
//     J(theta) = SUM over pixels, SUM over channels of film[pixel][channel]
//
// -- the same J shaders/includes/diff/bsdf_adjoint.glsl states it is the
// derivative of. Both renders use ONE seed and one sample per pixel, so this
// is not a comparison of two Monte Carlo means with a sampling error between
// them: it is the derivative of ONE realisation of the estimator, and the
// analytic side is the derivative of that same realisation. There is no
// variance term in the error budget at all. That is what CRN buys, and it is
// why one seed is not a weaker measurement than a hundred here -- a hundred
// seeds would be a hundred separate exact comparisons, not a tighter one.
//
// CRN holds only because nothing the perturbation touches changes the PATH.
// The albedo enters `f` and the throughput; it does NOT enter the sampled
// direction, the density, the MIS weights or the visibility, because the
// lobe-selection probability q is independent of the base colour at
// metallic == 0. GpuProbeContext::runWavefrontGradientProbe REFUSES to run
// outside that configuration, which is what keeps this paragraph a
// precondition rather than an assumption.
//
// ---------------------------------------------------------------------------
// THE STEP SIZE, DERIVED -- both error terms and where their sum is minimised
// ---------------------------------------------------------------------------
//
// The central difference D(h) = (J(a+h) - J(a-h)) / (2h) carries two errors
// that move in opposite directions:
//
//   * ROUNDOFF, from cancellation. Each film value is a float32 accumulated
//     on the GPU, so J is known to a relative error eps; the numerator is a
//     difference of two nearly equal numbers, so the absolute error on D is
//     about eps*(|J(a+h)| + |J(a-h)|) / (2h) ~ eps*|J|/h. It GROWS as h
//     shrinks.
//   * TRUNCATION, from the third and higher odd derivatives. In this
//     configuration J is an exact POLYNOMIAL in the albedo -- with a pure
//     Lambertian surface the throughput on arrival at bounce b is exactly
//     a^b and the direct estimate at that vertex is exactly linear in a, so
//     J(a) = SUM_{n=1..B} K_n a^n with every K_n >= 0 (they are products of
//     radiances, cosines, visibilities and MIS weights, none of which is
//     negative). It GROWS as h grows.
//
// Both are BOUNDED here rather than guessed, and the bounds are computed from
// the run's own numbers:
//
//   roundoffBound   = eps * (|J(a+h)| + |J(a-h)|) / (2h)
//   truncationBound = |J(a)| * MAX over n=1..B of  E_n(h) / a^n,
//       where E_n(h) = ((a+h)^n - (a-h)^n)/(2h) - n*a^(n-1)
//
// The truncation bound is exact-arithmetic-tight for this polynomial family:
// the true truncation error is SUM_n K_n E_n(h), every term is non-negative,
// and K_n a^n <= J(a) for each n because all the K are, so
// SUM_n K_n E_n <= (max_n E_n/a^n) * SUM_n K_n a^n = (max_n E_n/a^n) * J.
// Note E_1 = E_2 = 0 identically -- a central difference is EXACT on linear
// and quadratic terms -- so a one-bounce run has NO truncation error at all
// and the bound correctly reports zero.
//
// eps is `filmRelativeEps`, supplied by the caller and derived there rather
// than tuned here.
//
// The minimiser follows in closed form for the leading behaviour. With
// E_B(h)/a^B ~ (B choose 3) h^2 / a^3 for the top term and |J'| >= J/a,
//
//     E(h)/|J'|  ~  eps*a/h  +  c*h^2/a^2,
//
// so h* = (eps*a^3/(2c))^(1/3) -- a CUBE ROOT of the machine precision, which
// is the standard result and the reason the answer is around 1e-2 and not
// around 1e-7. The caller states the h it picked and the arithmetic that
// produced it; this function reports both bounds it actually computed so the
// choice is auditable against the run rather than only against the algebra.
//
// ---------------------------------------------------------------------------
// WHY THE TWO SIDES ARE INDEPENDENT
// ---------------------------------------------------------------------------
//
// The finite-difference side reads the FILM, written by the forward
// instantiation's hook. The analytic side reads the GRADIENT ARENA, written by
// the replay instantiation's hook, on a different run, into a different
// buffer, with a different push constant (the forward run is pushed
// gradArenaFloats = 0 and cannot write the arena at all). They share the
// traversal -- necessarily, that is the point of the traversal being one
// source -- and they share no accumulator, no constant and no host helper: the
// bounds above are computed from J and h alone and mention nothing the shader
// defines.
/// Sum of every float in a film, in double. The film's three channels are
/// bit-identical in this configuration (grey base colour, grey environment,
/// per-channel-identical factors), so this is 3x the luminance sum -- but it
/// is summed as it is stored rather than assuming that, so a per-channel
/// indexing error would move J.
double filmTotal(const std::vector<float>& film) {
    double total = 0.0;
    for (float v : film) total += static_cast<double>(v);
    return total;
}

/// Runs the three renders and fills `out`. Returns false on a dispatch
/// failure, a non-finite film, or a film readback of the wrong size -- never
/// on a comparison result, which is the caller's verdict to reach.
bool measureCrnAlbedoGradient(ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf,
                              uint32_t width, uint32_t height, uint32_t bounces,
                              const ohao::diff::WavefrontGenerateCamera& camera,
                              const std::vector<float>& positions,
                              const std::vector<uint32_t>& indices, float albedo, float step,
                              const ohao::diff::WavefrontScatterMaterial& material, uint32_t seed,
                              ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                              uint32_t gradArenaFloats, uint32_t gradAlbedoOffset,
                              double filmRelativeEps, CrnFdMeasurement& out) {
    out = CrnFdMeasurement{};

    const float aMinus = albedo - step;
    const float aPlus = albedo + step;
    const std::size_t expectedFloats = static_cast<std::size_t>(width) * height * 3u;

    struct Point {
        float value;
        double* total;
    };
    // ORDER MATTERS: the centre is LAST so that the arena the caller reads
    // back is the one the centre's replay run left, not a perturbed run's.
    const Point points[3] = {
        {aMinus, &out.jMinus}, {aPlus, &out.jPlus}, {albedo, &out.jCenter}};

    for (const Point& p : points) {
        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, width, height, bounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), p.value, material,
                                           seed, arena, gradArenaFloats, gradAlbedoOffset, film)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: gradient probe dispatch failed at albedo %.9g\n",
                         static_cast<double>(p.value));
            return false;
        }
        if (film.size() != expectedFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: gradient probe returned a film of %zu floats at "
                         "albedo %.9g, expected %zu\n",
                         film.size(), static_cast<double>(p.value), expectedFloats);
            return false;
        }
        for (float v : film) {
            if (!std::isfinite(v) || v < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: gradient probe film at albedo %.9g holds a "
                             "non-finite or negative value (%.9g). Everything downstream -- the "
                             "difference, both error bounds -- assumes a finite non-negative "
                             "film\n",
                             static_cast<double>(p.value), static_cast<double>(v));
                return false;
            }
        }
        *p.total = filmTotal(film);
    }

    const std::vector<float> gradBlock = arena.readback(ctx.allocator(), gradBlockIndex);
    if (gradBlock.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: gradient arena block %zu read back empty\n",
                     gradBlockIndex);
        return false;
    }
    out.analytic = static_cast<double>(gradBlock[0]);

    out.hActual = 0.5 * (static_cast<double>(aPlus) - static_cast<double>(aMinus));
    if (!(out.hActual > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the two perturbed albedos round to the same float "
                     "(%.9g), so the step is below the representable resolution at albedo %.9g "
                     "and the difference quotient is a division by zero\n",
                     static_cast<double>(aPlus), static_cast<double>(albedo));
        return false;
    }
    out.finiteDiff = (out.jPlus - out.jMinus) / (2.0 * out.hActual);
    out.absError = std::fabs(out.finiteDiff - out.analytic);
    out.relError = (std::fabs(out.analytic) > 0.0) ? out.absError / std::fabs(out.analytic) : 0.0;

    out.roundoffBound =
        filmRelativeEps * (std::fabs(out.jPlus) + std::fabs(out.jMinus)) / (2.0 * out.hActual);

    // max over n = 1..bounces of E_n(h)/a^n. See the header for why this
    // bounds SUM_n K_n E_n(h) given K_n >= 0.
    const double a = static_cast<double>(albedo);
    const double h = out.hActual;
    double worstShape = 0.0;
    for (uint32_t n = 1; n <= bounces; ++n) {
        const double en = (std::pow(a + h, static_cast<double>(n)) -
                           std::pow(a - h, static_cast<double>(n))) /
                              (2.0 * h) -
                          static_cast<double>(n) * std::pow(a, static_cast<double>(n) - 1.0);
        const double shape = std::fabs(en) / std::pow(a, static_cast<double>(n));
        if (shape > worstShape) worstShape = shape;
    }
    out.truncationBound = std::fabs(out.jCenter) * worstShape;
    out.errorBound = out.roundoffBound + out.truncationBound;
    return true;
}

// ===========================================================================
// STAGE 1 TASK 3 -- THE DETACHED FINITE-DIFFERENCE INSTRUMENT
// ===========================================================================
//
// WHY TASK 2's HARNESS ABOVE IS NOT THE INSTRUMENT FOR THIS.
//
// `measureCrnAlbedoGradient` is EXACT, and it is exact for a reason that does
// not survive a change of parameter. At `metallic = 0, specularWeight = 0` the
// lobe probability q is identically 0, so `diffBsdfSample` takes its
// cosine-hemisphere fast path and the drawn direction is a function of (u1,u2)
// and the shading frame ALONE. Nothing about the sampled direction depends on
// the albedo, so perturbing it re-renders the SAME path and the difference
// quotient is the derivative of one realisation of the estimator.
//
// Neither half of that holds for roughness or metallic. `bsdf.glsl` samples
// the GGX VNDF with `alpha = roughness^2`, and q -- the lobe choice -- is built
// from F0 and therefore from metallic. Perturbing either MOVES THE SAMPLED
// DIRECTION, at this vertex and at every earlier one. A naive common-random-
// number difference would therefore measure
//
//     (the derivative the adjoint computes) + (the movement of the directions)
//
// and the second term is one spec section 6.3 says is NOT differentiated:
// sampled directions are detached. A gate built on the naive difference would
// report a bias that is BY DESIGN, and would read as a broken adjoint.
//
// SO THE REFERENCE IS DETACHED TOO. `WavefrontLoop::Config`'s sampling-material
// override (pushed to the traversal as `sampleAlbedo`/`sampleRoughness`/
// `sampleMetallic`/`sampleSpecularWeight`, consumed by
// `bsdf.glsl`'s `diffBsdfSampleDetached`) holds every SAMPLING decision at
// theta_0 while the EVALUATED material moves to theta_0 +/- h. Because the
// frozen material also fixes every earlier bounce's draw, the whole path is
// held still -- not just this vertex's -- and the only things that move
// between the +h and -h renders are `f`, the mixture density, the partner
// density at the other strategy's direction, and everything downstream of
// those two (the MIS weights, the per-bounce weight, the throughput).
//
// That is exactly the function `shaders/includes/diff/bsdf_adjoint.glsl`
// differentiates, so the two sides compute one quantity two ways.
//
// AND THE CLAIM IS MEASURED, NOT ARGUED. Every render returns the FORWARD
// instantiation's binding-3 vertex trace as it stood after the last bounce,
// whose slots 6-8, 9-11 and 15 are the ray origin, ray direction and hit
// distance the traversal read out of path state. `traceMismatches` counts the
// floats of those seven slots that differ between a perturbed render and the
// centre one, ACROSS EVERY PATH. For a detached measurement it must be
// exactly 0; for a naive one it must not be, or the freeze was not doing
// anything and the comparison between them would be vacuous.
//
// ---------------------------------------------------------------------------
// WHY ONE SEED SUFFICES FOR CHECKS 39-40, AND NOT FOR CHECK 41
// ---------------------------------------------------------------------------
//
// With every path frozen at theta_0 by the sampling-material override, J(theta)
// is a DETERMINISTIC ARITHMETIC FUNCTION of theta: the same finite set of
// paths, the same hit points, the same directions at every bounce, for every
// theta the five renders visit. Nothing in the film is a random variable of
// theta any more -- the randomness (the seed, u1/u2/uLobe) only ever chose
// WHICH paths get walked, and the override holds that choice fixed too. So
// D(h) and the scattered analytic gradient are two arithmetic computations of
// the derivative of ONE number, not two estimators of an expectation, and the
// comparison below needs no Monte Carlo error term and no seed average: a
// single seed is not an under-sampled measurement of anything, because there
// is nothing left to sample once the paths are frozen.
//
// THIS IS WHY check 41's 40% seed-to-seed spread (the NAIVE quotient's, see
// its own header below) is not a problem for checks 39-40's one-seed gate --
// the naive measurement re-samples on every perturbed render, so ITS quotient
// really is an estimator with variance, while the detached one it is compared
// against is not. A reader who does not have this paragraph will eventually
// wonder why one gate needs three seeds and the other needs exactly one; this
// is the reason, and it does not follow from anything else in this file.
//
// ---------------------------------------------------------------------------
// THE ERROR BOUND, AND WHY ITS TRUNCATION HALF IS NOT TASK 2's
// ---------------------------------------------------------------------------
//
// Task 2 could bound truncation EXACTLY: with a pure Lambertian surface J is a
// polynomial in the albedo with non-negative coefficients, so
// max_n E_n(h)/a^n * J bounds SUM_n K_n E_n(h) term by term. J is NOT a
// polynomial in roughness or in metallic -- D, the Smith terms and the mixture
// density are rational and irrational in both -- so that argument has no
// counterpart here and copying its FORM while dropping its PROOF would be a
// bound that cannot fail.
//
// What is available instead is the leading term itself, MEASURED. A central
// difference has D(h) = J' + C h^2 + O(h^4), so
//
//     D(2h) - D(h) = 3 C h^2 + O(h^4)   =>   C h^2 = (D(2h) - D(h)) / 3
//
// -- Richardson's estimate of the truncation of D(h), computed from two step
// sizes of THIS run and nothing else. Five renders per measurement instead of
// three buys it: J at theta_0, theta_0 +/- h and theta_0 +/- 2h.
//
// It degrades in the right direction. When truncation is far below roundoff,
// |D(2h) - D(h)| is dominated by the two quotients' own cancellation error and
// the reported "truncation" is really a second roundoff estimate -- larger
// than the truth, so the bound is conservative, never optimistic. When
// truncation dominates it is the actual leading term. What it is NOT is a
// bound on the h^4 and higher terms; the step sizes below are derived to put
// those far under the h^2 term, and the check reports both halves so that a
// case where they are comparable is visible rather than hidden.
//
// The roundoff half is Task 2's unchanged, including its eps: a film value is
// still a sum over bounces of a product of about six float32 factors, so
// `kFilmRelativeEps = 2e-6` bounds its relative accuracy for the same reason
// and by the same arithmetic.
/// Compares the (origin, dir, hitT) slots of two vertex traces and returns the
/// number of floats that differ, compared as BITS (memcmp of the float), not
/// through a tolerance: two renders that walked the same path wrote the same
/// bytes, and "nearly the same path" is not a thing this instrument may accept.
///
/// The throughput slots (12-14) are deliberately NOT compared: the throughput
/// is the quantity the perturbation is SUPPOSED to move. Comparing it would
/// make the check fail for the very effect it exists to permit.
std::size_t traceGeometryMismatches(const std::vector<float>& a, const std::vector<float>& b,
                                    uint32_t capacity) {
    if (a.size() != b.size()) return a.size() + b.size();
    const uint32_t slots[7] = {
        ohao::diff::kTraceSlotOrigin + 0u, ohao::diff::kTraceSlotOrigin + 1u,
        ohao::diff::kTraceSlotOrigin + 2u, ohao::diff::kTraceSlotDir + 0u,
        ohao::diff::kTraceSlotDir + 1u,    ohao::diff::kTraceSlotDir + 2u,
        ohao::diff::kTraceSlotHitT};
    std::size_t mismatches = 0;
    for (uint32_t p = 0; p < capacity; ++p) {
        const std::size_t base = static_cast<std::size_t>(p) * ohao::diff::kDebugDrawFloats;
        for (uint32_t s : slots) {
            const float x = a[base + s];
            const float y = b[base + s];
            if (std::memcmp(&x, &y, sizeof(float)) != 0) ++mismatches;
        }
    }
    return mismatches;
}

/// Runs the five renders and fills `out`. `param` is DIFF_PARAM_ROUGHNESS (1)
/// or DIFF_PARAM_METALLIC (2) and selects which field of `material` the step
/// is applied to. Returns false on a dispatch failure or a bad film -- never on
/// a comparison result, which is the caller's verdict to reach.
bool measureDetachedGgxGradient(ohao::diff::GpuProbeContext& ctx,
                                ohao::diff::WavefrontBuffers& wf, uint32_t width, uint32_t height,
                                uint32_t bounces, const ohao::diff::WavefrontGenerateCamera& camera,
                                const std::vector<float>& positions,
                                const std::vector<uint32_t>& indices, float albedo,
                                const ohao::diff::WavefrontScatterMaterial& material,
                                uint32_t param, float step, bool freezeSampling, uint32_t seed,
                                ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                                uint32_t gradArenaFloats, uint32_t gradOffset,
                                double filmRelativeEps, GgxFdMeasurement& out) {
    out = GgxFdMeasurement{};
    out.sampledDetached = freezeSampling;

    const uint32_t capacity = width * height;
    const std::size_t expectedFloats = static_cast<std::size_t>(capacity) * 3u;

    auto perturbed = [&](float delta) {
        ohao::diff::WavefrontScatterMaterial m = material;
        if (param == 1u) {
            m.roughness += delta;
        } else {
            m.metallic += delta;
        }
        return m;
    };

    struct Point {
        float delta;
        double* total;
        std::vector<float>* trace;
    };
    std::vector<float> traces[5];
    // ORDER MATTERS: the centre is LAST so the arena the caller reads back is
    // the one the CENTRE's replay run left, not a perturbed run's. Task 2's
    // harness makes the same choice for the same reason.
    const Point points[5] = {
        {-2.0f * step, &out.jMinus2, &traces[0]}, {-step, &out.jMinus, &traces[1]},
        {step, &out.jPlus, &traces[2]},           {2.0f * step, &out.jPlus2, &traces[3]},
        {0.0f, &out.jCenter, &traces[4]},
    };

    for (const Point& p : points) {
        const ohao::diff::WavefrontScatterMaterial m = perturbed(p.delta);
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = param;
        options.freezeSampling = freezeSampling;
        options.samplingAlbedo = albedo;
        // The UNPERTURBED material, on every one of the five renders. That is
        // the whole of the instrument.
        options.samplingMaterial = material;
        options.outForwardTrace = p.trace;

        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, width, height, bounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), albedo, m, seed,
                                           arena, gradArenaFloats, gradOffset, film, options)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: detached gradient probe dispatch failed at "
                         "roughness %.9g / metallic %.9g\n",
                         static_cast<double>(m.roughness), static_cast<double>(m.metallic));
            return false;
        }
        if (film.size() != expectedFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: detached gradient probe returned a film of %zu "
                         "floats, expected %zu\n",
                         film.size(), expectedFloats);
            return false;
        }
        double total = 0.0;
        for (float v : film) {
            if (!std::isfinite(v) || v < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: detached gradient probe film holds a "
                             "non-finite or negative value (%.9g)\n",
                             static_cast<double>(v));
                return false;
            }
            total += static_cast<double>(v);
        }
        *p.total = total;
    }

    for (int i = 0; i < 4; ++i) {
        out.traceMismatches += traceGeometryMismatches(traces[i], traces[4], capacity);
    }

    const std::vector<float> gradBlock = arena.readback(ctx.allocator(), gradBlockIndex);
    if (gradBlock.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: gradient arena block %zu read back empty\n",
                     gradBlockIndex);
        return false;
    }
    out.analytic = static_cast<double>(gradBlock[0]);

    // The ACTUAL float steps, recovered the way Task 2's harness recovers
    // its own: from the two perturbed values as floats, so the representation
    // error of theta +/- h cancels out of the quotient exactly.
    const float base = (param == 1u) ? material.roughness : material.metallic;
    const float plus = base + step;
    const float minus = base - step;
    const float plus2 = base + 2.0f * step;
    const float minus2 = base - 2.0f * step;
    out.hActual = 0.5 * (static_cast<double>(plus) - static_cast<double>(minus));
    out.hActual2 = 0.5 * (static_cast<double>(plus2) - static_cast<double>(minus2));
    if (!(out.hActual > 0.0) || !(out.hActual2 > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the perturbed parameter values round to the same "
                     "float at base %.9g, step %.9g -- the difference quotient is a division by "
                     "zero\n",
                     static_cast<double>(base), static_cast<double>(step));
        return false;
    }

    out.finiteDiff = (out.jPlus - out.jMinus) / (2.0 * out.hActual);
    out.finiteDiff2h = (out.jPlus2 - out.jMinus2) / (2.0 * out.hActual2);
    out.absError = std::fabs(out.finiteDiff - out.analytic);
    out.relError = (std::fabs(out.analytic) > 0.0) ? out.absError / std::fabs(out.analytic) : 0.0;

    out.roundoffBound =
        filmRelativeEps * (std::fabs(out.jPlus) + std::fabs(out.jMinus)) / (2.0 * out.hActual);
    // Richardson: D(2h) - D(h) = 3 C h^2 + O(h^4), and C h^2 is the truncation
    // of D(h). See this harness's header for why this replaces Task 2's exact
    // polynomial bound and in which direction it errs.
    out.truncationBound = std::fabs(out.finiteDiff2h - out.finiteDiff) / 3.0;
    out.errorBound = out.roundoffBound + out.truncationBound;
    return true;
}

// ===========================================================================
// STAGE 1 TASK 4 -- d(film)/d(EMISSION), THE PLUMBING CHECK
// ===========================================================================
//
// WHY PLAIN CRN (Task 2's harness, not Task 3's detached one) IS THE
// INSTRUMENT, AND HOW THAT IS MEASURED RATHER THAN ASSUMED.
//
// Task 3's detached instrument exists because perturbing roughness or
// metallic moves the sampled direction: `bsdf.glsl` reads both to build the
// GGX VNDF's `alpha` and the lobe-selection probability `q`. Emission is read
// by NEITHER `diffBsdfSample`/`diffBsdfSampleDetached` (roughness/metallic's
// callee) NOR `sampleEnvMap` -- grep both files and `pc.emission` appears in
// neither -- so a +/-h perturbation of it changes no draw and moves no
// direction at any bounce. That is the SAME property that made Task 2's
// plain common-random-number harness exact for the albedo at metallic 0, and
// this function measures it exactly the way Task 3 measured the ABSENCE of
// that property for roughness/metallic: it captures the forward run's
// binding-3 vertex trace at emission-h, emission and emission+h (via
// `WavefrontGradientOptions::outForwardTrace`, the same field Task 3's
// harness uses) and runs `traceGeometryMismatches` -- the identical bit-exact
// origin/direction/hitT comparison, unmodified -- between the two perturbed
// traces and the centre one. `out.traceMismatches` is the sum; the caller
// requires it to be exactly 0, and it is 0 for a reason this function's
// comment states and the check's own run confirms rather than assumes.
//
// THIS IS A SIBLING OF `measureCrnAlbedoGradient`, NOT A THIRD HARNESS. Both
// run three common-random-number renders under `runWavefrontGradientProbe`
// and reduce to a `CrnFdMeasurement`; the reason this is a separate function
// rather than a call to that one is that `measureCrnAlbedoGradient` perturbs
// its `albedo` PARAMETER (the function argument that becomes
// `runWavefrontGradientProbe`'s positional `albedo`), while emission is
// threaded through `WavefrontGradientOptions::emission` instead -- `albedo`
// and `material` here are the FIXED, unperturbed scene, exactly as Task 3's
// `measureDetachedGgxGradient` holds `albedo` fixed while perturbing a field
// of `material`. "A scene whose only parameter is emission" (the brief's
// phrase for Step 1) means precisely that: `albedo`/`material` do not move
// across the three renders below, only `emission` does.
//
// THE ERROR BOUND HAS NO TRUNCATION TERM -- not a measured near-zero one, an
// ABSENT one, and that is the mathematical content Step 1 asks to be derived
// and stated. `J(emission) = A + emission * B` with
// `A = SUM_b T_b * Lr_b` and `B = SUM_b T_b`, NEITHER of which depends on
// emission (see bsdf_adjoint.glsl's "STAGE 1 TASK 4" banner for why every
// other term of the recursion is identically zero for this parameter). A
// function that is EXACTLY linear has E_n(h) == 0 for its one nonzero
// term (n=1, a central difference is exact on a linear function) and no
// n >= 2 term to have any curvature at all -- unlike the albedo, whose
// linearity held only up to Task 2's E_1/E_2 (the polynomial's own 3rd term
// gave it a genuine, if small, truncation error from 3 bounces on).
// Consequently `out.truncationBound` below is not computed by any
// Richardson estimate or polynomial bound; it is set to the literal `0.0`
// the derivation gives. This is not asserted blindly: the comparison this
// feeds (`|FD - analytic| <= roundoffBound`, with NO SLACK for anything a
// truncation term could have absorbed) is run at three bounce counts with a
// resolution the caller checks is far inside the pre-registered limit --
// which a genuinely nonzero cubic/quadratic term would have no room to hide
// from.
bool measureCrnEmissionGradient(ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf,
                                uint32_t width, uint32_t height, uint32_t bounces,
                                const ohao::diff::WavefrontGenerateCamera& camera,
                                const std::vector<float>& positions,
                                const std::vector<uint32_t>& indices, float albedo,
                                const ohao::diff::WavefrontScatterMaterial& material,
                                float emission, float step, uint32_t seed,
                                ohao::diff::GradientArena& arena, std::size_t gradBlockIndex,
                                uint32_t gradArenaFloats, uint32_t gradEmissionOffset,
                                double filmRelativeEps, CrnFdMeasurement& out) {
    out = CrnFdMeasurement{};

    const uint32_t capacity = width * height;
    const float eMinus = emission - step;
    const float ePlus = emission + step;
    const std::size_t expectedFloats = static_cast<std::size_t>(width) * height * 3u;

    struct Point {
        float value;
        double* total;
        std::vector<float>* trace;
    };
    std::vector<float> traces[3];
    // ORDER MATTERS: the centre is LAST so that the arena the caller reads
    // back is the one the centre's replay run left, not a perturbed run's --
    // the same reason `measureCrnAlbedoGradient` orders its points this way.
    const Point points[3] = {{eMinus, &out.jMinus, &traces[0]},
                             {ePlus, &out.jPlus, &traces[1]},
                             {emission, &out.jCenter, &traces[2]}};

    for (const Point& p : points) {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 3u;  // DIFF_PARAM_EMISSION (bsdf_adjoint.glsl)
        options.emission = p.value;
        options.outForwardTrace = p.trace;

        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, width, height, bounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), albedo, material,
                                           seed, arena, gradArenaFloats, gradEmissionOffset, film,
                                           options)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: emission gradient probe dispatch failed at "
                         "emission %.9g\n",
                         static_cast<double>(p.value));
            return false;
        }
        if (film.size() != expectedFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: emission gradient probe returned a film of %zu "
                         "floats at emission %.9g, expected %zu\n",
                         film.size(), static_cast<double>(p.value), expectedFloats);
            return false;
        }
        for (float v : film) {
            if (!std::isfinite(v) || v < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: emission gradient probe film at emission "
                             "%.9g holds a non-finite or negative value (%.9g)\n",
                             static_cast<double>(p.value), static_cast<double>(v));
                return false;
            }
        }
        *p.total = filmTotal(film);
    }

    // THE CRN-VALIDITY MEASUREMENT. See the header: this is what makes "plain
    // CRN is exact for emission" a measured claim rather than an inherited
    // one. traces[0]/traces[1] are the perturbed renders, traces[2] the
    // centre; every one of the seven geometry slots (origin, dir, hitT) must
    // be bit-identical across all `capacity` paths.
    out.traceMismatches = traceGeometryMismatches(traces[0], traces[2], capacity) +
                          traceGeometryMismatches(traces[1], traces[2], capacity);

    const std::vector<float> gradBlock = arena.readback(ctx.allocator(), gradBlockIndex);
    if (gradBlock.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: gradient arena block %zu read back empty\n",
                     gradBlockIndex);
        return false;
    }
    out.analytic = static_cast<double>(gradBlock[0]);

    out.hActual = 0.5 * (static_cast<double>(ePlus) - static_cast<double>(eMinus));
    if (!(out.hActual > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the two perturbed emissions round to the same float "
                     "(%.9g), so the step is below the representable resolution at emission %.9g "
                     "and the difference quotient is a division by zero\n",
                     static_cast<double>(ePlus), static_cast<double>(emission));
        return false;
    }
    out.finiteDiff = (out.jPlus - out.jMinus) / (2.0 * out.hActual);
    out.absError = std::fabs(out.finiteDiff - out.analytic);
    out.relError = (std::fabs(out.analytic) > 0.0) ? out.absError / std::fabs(out.analytic) : 0.0;

    out.roundoffBound =
        filmRelativeEps * (std::fabs(out.jPlus) + std::fabs(out.jMinus)) / (2.0 * out.hActual);
    // See the function header: J(emission) is EXACTLY linear, so there is no
    // truncation term to bound -- 0.0 literally, not a measured near-zero.
    out.truncationBound = 0.0;
    out.errorBound = out.roundoffBound + out.truncationBound;
    return true;
}

// ===========================================================================
// STAGE 1 TASK 5: THE EMISSION TEXTURE'S HARNESS
// ===========================================================================
//
// Fills `WavefrontGradientOptions`' seven Task 5 fields, so that the three
// places below that configure an emission-texture render cannot drift into
// configuring three slightly different ones.
ohao::diff::WavefrontGradientOptions emissionTextureOptions(
    const std::vector<float>& texels, const ohao::diff::ParamShape& shape, float uvScaleU,
    float uvScaleV, float uvBiasU, float uvBiasV) {
    ohao::diff::WavefrontGradientOptions options;
    options.diffParam = 4u;  // DIFF_PARAM_EMISSION_TEXTURE (bsdf_adjoint.glsl)
    options.emissionTexture = texels;
    options.emissionTexWidth = shape.width;
    options.emissionTexHeight = shape.height;
    options.emissionTexChannels = shape.channels;
    options.emissionUvScaleU = uvScaleU;
    options.emissionUvScaleV = uvScaleV;
    options.emissionUvBiasU = uvBiasU;
    options.emissionUvBiasV = uvBiasV;
    return options;
}

/// THE HOST'S BILINEAR FOOTPRINT. Written HERE, from the convention, not
/// called out of the shader -- it is what check 44 predicts the touched arena
/// elements from, and a prediction taken from the thing under test predicts
/// nothing.
///
/// It matches `diffBilinearFootprint` (bsdf_adjoint.glsl) by CONSTRUCTION of
/// the same convention (texel centres at (i+0.5)/size, clamp at the border)
/// and is computed in `float`, not `double`, so that a uv sitting near a cell
/// boundary could not have the two sides land on different texels for a
/// rounding reason. Check 44 chooses a uv well inside a cell besides.
HostBilinearFootprint hostBilinearFootprint(float u, float v, std::uint32_t width,
                                            std::uint32_t height) {
    const auto clampTexel = [](float coord, std::uint32_t size) -> std::uint32_t {
        const float lo = 0.0f;
        const float hi = static_cast<float>(size) - 1.0f;
        const float c = (coord < lo) ? lo : ((coord > hi) ? hi : coord);
        return static_cast<std::uint32_t>(c);
    };
    const float cx = u * static_cast<float>(width) - 0.5f;
    const float cy = v * static_cast<float>(height) - 0.5f;
    const float bx = std::floor(cx);
    const float by = std::floor(cy);
    const float tx = cx - bx;
    const float ty = cy - by;
    HostBilinearFootprint fp{};
    fp.x0 = clampTexel(bx, width);
    fp.x1 = clampTexel(bx + 1.0f, width);
    fp.y0 = clampTexel(by, height);
    fp.y1 = clampTexel(by + 1.0f, height);
    fp.w00 = (1.0f - tx) * (1.0f - ty);
    fp.w10 = tx * (1.0f - ty);
    fp.w01 = (1.0f - tx) * ty;
    fp.w11 = tx * ty;
    return fp;
}

// A PER-TEXEL-ELEMENT finite difference, under PLAIN common random numbers.
//
// THE SIBLING OF `measureCrnEmissionGradient`, AND WHY IT IS A SEPARATE
// FUNCTION. That one perturbs `WavefrontGradientOptions::emission`, a scalar;
// this one perturbs ONE FLOAT of `WavefrontGradientOptions::emissionTexture`,
// which the probe re-uploads on every render. Everything else -- three CRN
// renders, the centre LAST so the arena the caller reads is the centre's, the
// vertex-trace capture and `traceGeometryMismatches` between the perturbed
// runs and the centre, the roundoff-only error bound -- is that function's,
// unchanged, and for that function's reasons.
//
// PLAIN CRN, NOT TASK 3'S DETACHED INSTRUMENT, and the argument is
// STRUCTURAL. The emission texture is read by `diffEmissionAt`
// (bsdf_adjoint.glsl) and by nothing else in the traversal's translation
// unit; `diffBsdfSample`/`diffBsdfSampleDetached` and `sampleEnvMap` take no
// emission argument of any kind and never touch binding 11. So perturbing a
// texel moves no draw and no direction at any bounce, which is precisely the
// property a plain common-random-number difference quotient needs -- the same
// property `pc.emission` has and roughness/metallic do not. The trace
// comparison below MEASURES it too (`out.traceMismatches`, which the caller
// requires to be exactly 0), but that record is overwritten each bounce and
// so covers bounces 0..N-2; the structural argument is what actually closes
// it, and the measurement is corroboration.
//
// THE ERROR BOUND HAS NO TRUNCATION TERM, for `measureCrnEmissionGradient`'s
// reason and with one more step of the argument. The forward hook writes
// `throughput * (Lr + E(uv))` and `E` is a LINEAR function of the texels
// (`SUM_i w_i * texel_i`, with weights that depend on uv alone), so
//
//     J(texels) = A + SUM_k B_k * texel_k,
//     A = SUM_b T_b*Lr_b,  B_k = SUM_b T_b * w_k(uv_b)
//
// and neither A nor any B_k depends on any texel: the throughput recursion
// and the MIS-combined `Lr` never read binding 11. J is therefore EXACTLY
// LINEAR in every element separately, a central difference is exact at every
// step size, and `out.truncationBound` is the literal 0.0 the derivation
// gives rather than a measured near-zero.
bool measureCrnEmissionTexelGradient(
    ohao::diff::GpuProbeContext& ctx, ohao::diff::WavefrontBuffers& wf, uint32_t width,
    uint32_t height, uint32_t bounces, const ohao::diff::WavefrontGenerateCamera& camera,
    const std::vector<float>& positions, const std::vector<uint32_t>& indices, float albedo,
    const ohao::diff::WavefrontScatterMaterial& material, const std::vector<float>& baseTexels,
    const ohao::diff::ParamShape& shape, float uvScaleU, float uvScaleV, float uvBiasU,
    float uvBiasV, uint32_t element, float step, uint32_t seed, ohao::diff::GradientArena& arena,
    std::size_t gradBlockIndex, uint32_t gradArenaFloats, uint32_t gradTexOffset,
    double filmRelativeEps, CrnFdMeasurement& out) {
    out = CrnFdMeasurement{};

    const uint32_t capacity = width * height;
    const std::size_t expectedFloats = static_cast<std::size_t>(width) * height * 3u;
    if (element >= baseTexels.size()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: emission-texel gradient asked for element %u of a "
                     "%zu-float texture\n",
                     element, baseTexels.size());
        return false;
    }
    const float base = baseTexels[element];
    const float eMinus = base - step;
    const float ePlus = base + step;

    struct Point {
        float value;
        double* total;
        std::vector<float>* trace;
    };
    std::vector<float> traces[3];
    // ORDER MATTERS: the centre is LAST so the arena the caller reads back is
    // the one the centre's replay run left -- `measureCrnAlbedoGradient`'s
    // and `measureCrnEmissionGradient`'s reason exactly.
    const Point points[3] = {{eMinus, &out.jMinus, &traces[0]},
                             {ePlus, &out.jPlus, &traces[1]},
                             {base, &out.jCenter, &traces[2]}};

    for (const Point& p : points) {
        std::vector<float> texels = baseTexels;
        texels[element] = p.value;
        ohao::diff::WavefrontGradientOptions options =
            emissionTextureOptions(texels, shape, uvScaleU, uvScaleV, uvBiasU, uvBiasV);
        options.outForwardTrace = p.trace;

        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, width, height, bounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), albedo, material,
                                           seed, arena, gradArenaFloats, gradTexOffset, film,
                                           options)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: emission-texel gradient probe dispatch failed at "
                         "element %u = %.9g\n",
                         element, static_cast<double>(p.value));
            return false;
        }
        if (film.size() != expectedFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: emission-texel gradient probe returned a film of "
                         "%zu floats, expected %zu\n",
                         film.size(), expectedFloats);
            return false;
        }
        for (float fv : film) {
            if (!std::isfinite(fv) || fv < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: emission-texel gradient probe film holds a "
                             "non-finite or negative value (%.9g)\n",
                             static_cast<double>(fv));
                return false;
            }
        }
        *p.total = filmTotal(film);
    }

    out.traceMismatches = traceGeometryMismatches(traces[0], traces[2], capacity) +
                          traceGeometryMismatches(traces[1], traces[2], capacity);

    const std::vector<float> gradBlock = arena.readback(ctx.allocator(), gradBlockIndex);
    if (gradBlock.size() <= static_cast<std::size_t>(element)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: gradient arena block %zu read back %zu floats, too "
                     "few to hold element %u\n",
                     gradBlockIndex, gradBlock.size(), element);
        return false;
    }
    out.analytic = static_cast<double>(gradBlock[element]);

    out.hActual = 0.5 * (static_cast<double>(ePlus) - static_cast<double>(eMinus));
    if (!(out.hActual > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the two perturbed texel values round to the same "
                     "float (%.9g), so the difference quotient is a division by zero\n",
                     static_cast<double>(ePlus));
        return false;
    }
    out.finiteDiff = (out.jPlus - out.jMinus) / (2.0 * out.hActual);
    out.absError = std::fabs(out.finiteDiff - out.analytic);
    out.relError = (std::fabs(out.analytic) > 0.0) ? out.absError / std::fabs(out.analytic) : 0.0;
    out.roundoffBound =
        filmRelativeEps * (std::fabs(out.jPlus) + std::fabs(out.jMinus)) / (2.0 * out.hActual);
    // Exactly linear in every texel separately -- see the header.
    out.truncationBound = 0.0;
    out.errorBound = out.roundoffBound + out.truncationBound;
    return true;
}

}  // namespace ohao::diff::probe
