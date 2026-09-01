// Direct-lighting and film checks 29-32: NEE/BSDF/MIS agreement, the
// per-sample MIS partition, the envIntegral/pdfEnvMap/routing claims, and
// radiance accumulation into the film.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_nee_film.hpp"

#include "probe/oracle_bsdf.hpp"

#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkNeeMisAndRouting(ohao::diff::GpuProbeContext& ctx) {
    // ------------------------------------------------------------------
    // 29-31. NEXT-EVENT ESTIMATION AND MIS (Stage 0b-2b Task 4).
    // ------------------------------------------------------------------
    //
    // wf_scatter.comp now estimates the direct-lighting integral at each hit
    // point TWICE, by two different sampling strategies, and combines them
    // with the balance heuristic:
    //
    //   I = integral over the sphere of f(N,V,w) max(0, N.w) L(w) V(w) dw
    //
    //   strategy E ("next event"): w ~ p_E, the environment's
    //       sin(theta)-weighted luminance (env_sampling.glsl's sampleEnvMap
    //       -- the SAME call, on the SAME sample, that binding 6 records and
    //       check 24 chi-squares; see the routing tie in check 31)
    //   strategy B ("BSDF sampling"): w ~ p_B, diffBsdfSample's own mixture
    //
    // ------------------------------------------------------------------
    // THE ORACLE: strategy agreement, not a re-implemented integrand
    // ------------------------------------------------------------------
    //
    // Each strategy's single-sample estimator f*cos*L*V/p_own is unbiased
    // for I on its own. So are their MIS combination and, separately, each
    // half of it. Three estimators of one truth -- and the truth is never
    // computed here. A host oracle that re-evaluated f, L and V would share
    // whatever misconception the shader has about any of them; two
    // independent SAMPLERS of the same integral share nothing but the
    // integral itself.
    //
    // THE ONE THING THEIR EXPECTATIONS DO NOT SHARE, and it is derived, not
    // waved away. env_sampling.glsl returns TEXEL CENTRES (checks 25/27
    // pin that), so strategy E is a midpoint quadrature of the piecewise-
    // constant environment, while strategy B draws continuously and
    // integrates the same piecewise-constant map EXACTLY. Those two numbers
    // are not equal, and pretending they were would be a bound that passes
    // its own perturbation. (site/content/units/sampling/env-cdf.md states
    // the same thing about the RT pipeline's env-NEE block: sampleEnvMap
    // "returns equirectPixelToDir(x, y, W, H) unmodified -- the exact texel
    // centre, with no intra-texel jitter", so the strategy's expectation is
    // a midpoint quadrature "rather than the integral". The derivation
    // below is what that costs, in closed form, for this scene.)
    //
    // WHICH RADIANCE STRATEGY B MULTIPLIES IN IS PART OF THIS DERIVATION,
    // and it was wrong for one commit. The BSDF side has no radiance image
    // either; it recovers L by inverting a density. env_sampling.glsl's
    // pdfEnvMap is NOT the texel density off a texel centre -- it is that
    // density times sin(theta_centre)/sin(theta_query) -- so inverting IT
    // yields L*sin(theta_centre)/sin(theta_query), and the stray sin(theta)
    // then cancels against the solid-angle measure. The estimator that
    // results integrates a HALF-WIDTH midpoint rule, whose closed-form
    // ratio is sinc(pi/(2*envH)), not sinc(pi/envH). At envH = 64 the two
    // constants differ by 3.0e-4 relative -- 0.06 of one standard error of
    // D1 below, i.e. invisible -- while at envH = 8 they differ by 1.9%,
    // about 3.6 standard errors, and this check would have failed for a
    // correct-looking reason. wf_scatter.comp now calls pdfEnvMapTexel for
    // the radiance and keeps pdfEnvMap for the MIS weight (where the sin
    // ratio cancels out of the balance heuristic anyway), so Y_i is
    // albedo * L_texel -- BOUNDED, which also trims the heavy right tail
    // the z-score discussion below assumes away. Check 31 asserts the
    // recovered value against the environment image per sample, so the
    // choice is measured rather than derived-and-hoped.
    //
    // The scene is chosen so the gap has a CLOSED FORM. The surface normal
    // is +Y, the equirectangular pole, so the cosine factor is
    // max(0, cos theta): a function of the ROW alone, with no azimuthal
    // dependence, and (for even envH) a horizon that falls exactly on a row
    // boundary rather than cutting through a row. Over one row
    // [theta1, theta2] of width dtheta, with L constant on the row,
    //
    //   exact   = L dphi * integral of cos(theta) sin(theta) dtheta
    //           = L dphi * cos(theta_c) sin(theta_c) sin(dtheta)
    //   midpoint= L dphi * dtheta cos(theta_c) sin(theta_c)
    //
    // using cos(theta)sin(theta) = sin(2 theta)/2 and
    // cos(2 theta_1) - cos(2 theta_2) = 2 sin(2 theta_c) sin(dtheta). Their
    // ratio is sin(dtheta)/dtheta -- INDEPENDENT of the row, of L, and of
    // the map's azimuthal structure -- so summing over every texel,
    //
    //   E[strategy B] = kappa * E[strategy E],   kappa = sinc(pi/envH).
    //
    // At envH = 64 that is 0.99959845, a 4.02e-4 relative offset. Nothing is
    // fitted: kappa is a trigonometric identity, and it is the ONLY host
    // input the agreement check takes.
    //
    // ------------------------------------------------------------------
    // 29. The bound, derived
    // ------------------------------------------------------------------
    //
    // Every path shades the same normal (+Y) with the same Lambertian BSDF
    // (specularWeight 0, metallic 0 -- f = albedo/pi, independent of the
    // view direction, so the per-pixel view variation does not make the
    // samples non-identically-distributed) and the same unoccluded upper
    // hemisphere. The N = 49152 per-path records are therefore i.i.d. draws
    // of one scalar estimator each, and the comparisons are PAIRED per
    // sample:
    //
    //   D1_i = Y_i - kappa X_i        E[D1] = 0 exactly
    //                                 (exactly, for any even envH, ONLY
    //                                  because Y multiplies in the texel
    //                                  radiance -- see above)
    //   D2_i = Z_i - Y_i              E[D2] = delta   (below)
    //   D3_i = Z_i - X_i              E[D3] = delta - (1-kappa) E[X]
    //
    // with X the next-event-only estimator, Y the BSDF-only one and Z their
    // MIS combination. The bound on each is z * s_D / sqrt(N) using THAT
    // difference's own sample standard deviation -- exact whatever the
    // correlation between X_i and Y_i, which is why the comparison is
    // paired rather than a difference of two independent means.
    //
    // delta is the MIS estimator's own share of the same midpoint-vs-exact
    // gap: its next-event half is the same midpoint sum damped by
    // w_E in [0,1], its BSDF half is exact, so
    // delta = Mid[w_E g] - Exact[w_E g]. It is allowed for at
    // (1 - kappa) * mean(X) -- the UNDAMPED gap, i.e. the same integral with
    // w_E replaced by 1. Measured by numerical quadrature at four
    // resolutions (envH = 8, 16, 32, 64) the true ratio delta/((1-kappa)A)
    // is 0.70, 0.77, 0.80, 0.81 -- always below 1, so the allowance is
    // conservative by about 20%. At envH = 64 the allowance is 4.02e-4
    // relative (0.001571 absolute), against a D2 standard error of 0.014480
    // and a total D2 bound of 0.088453: the systematic term is a TENTH OF
    // THE STANDARD ERROR and under 2% of the bound. (It is not "a tenth of
    // the bound"; that is what this comment used to say, and the report
    // said 2%. The bound is what z*se makes it, and the systematic is a
    // small correction on top.)
    //
    // z = 6. The nominal two-sided Gaussian rate is 2e-9; the HONEST rate is
    // Berry-Esseen's, and for this estimator's third absolute moment that
    // bound is 2.7e-3 whatever z is, so z buys margin against the
    // perturbation rather than against the tail. The empirical justification
    // is the one that matters: over 60 independent replications of exactly
    // these estimators at exactly this N, simulated on the host before any
    // of this was written, the largest |D| observed was 2.5 standard errors
    // -- comfortably under half the threshold -- while the Step 5
    // perturbation (inverting ONE misBalanceHeuristic call's arguments)
    // pushes D2 to 1.114377 against a bound of 0.088453 and D3 to 1.150805
    // against 0.048983. The bound rejects the perturbation by 12.6x on D2
    // and 23.4x on D3, and admits the truth by a factor of 2.4. (Neither
    // factor is 26; that number was in this comment and in the report and
    // matched neither measurement.)
    //
    // NON-VACUITY. All three estimators are asserted strictly positive, and
    // the count of samples that contributed anything is printed. An
    // unwritten (all-zero) sink would make all three agree perfectly at 0,
    // which is exactly the failure mode a pure agreement check cannot see.
    // 256x192 = 49152 paths, ONE scatter dispatch. Every path is an
    // independent RNG stream (streams are keyed by pixel index), so the
    // sample count comes from the image rather than from repeating
    // dispatches -- which is also the configuration the integrator
    // actually runs in.
    constexpr uint32_t kW = 256;
    constexpr uint32_t kH = 192;
    constexpr uint32_t kCapacity = kW * kH;  // 49152
    constexpr uint32_t kIterationSeed = 4040404u;
    constexpr float kAlbedo = 1.0f;

    // envH must be EVEN for the horizon to land on a row boundary (see
    // the kappa derivation), and 64 is where the derived systematic
    // offset drops an order of magnitude below the Monte Carlo error.
    constexpr uint32_t kEnvW = 128;
    constexpr uint32_t kEnvH = 64;
    static_assert(kEnvH % 2u == 0u,
                 "the closed-form midpoint/exact ratio needs the +Y horizon to fall on a "
                 "row boundary, which requires an even envH");
    // RESOLUTION GUARD. Evenness is what the kappa IDENTITY needs and
    // it is not what this CHECK needs: three separate things below stop
    // holding outside a band of envH, none of them visible at 64, and
    // the failure mode of each is a spurious FAIL rather than a silent
    // pass. Guarding the band is cheaper than rediscovering them.
    //
    //   * UPPER BOUND, 128. The smallest |cos(theta)| any row CENTRE
    //     takes is sin(pi/(2*envH)) -- 0.0245 at envH = 64, 0.0123 at
    //     128. Two things are compared against a 1e-4 floor at that
    //     scale: check 31 compares diffBsdfEval's pdf at the light
    //     sample against max(0, d.y)/pi UNCONDITIONALLY (valid only
    //     while no texel centre reaches bsdf.glsl's DIFF_BSDF_MIN_COS
    //     grazing branch), and env_sampling.glsl clamps its own
    //     sin(theta) at 1e-4 (which must never engage at a texel
    //     centre, or pdfEnvMapTexel stops being the texel density and
    //     check 31's radiance tie starts failing near the poles). At
    //     128 both keep two orders of magnitude of margin; past it the
    //     margin erodes and these become conditional checks that this
    //     code does not make conditional.
    //   * LOWER BOUND, 8. D2/D3 allow for delta at (1 - kappa)*mean(X).
    //     That allowance is justified ONLY by numerical quadrature at
    //     envH = 8, 16, 32, 64 (ratios 0.70, 0.77, 0.80, 0.81), not by
    //     proof. Below 8 nothing has measured it, and (1 - kappa) grows
    //     like envH^-2, so it stops being a small correction and starts
    //     being the bound.
    //
    // Note what this guard is NOT for: kappa itself is exact for every
    // even envH now that strategy B recovers texel radiance. Before
    // that fix the correct constant was sinc(pi/(2*envH)) and this
    // check passed at envH = 64 only because the two agree to 3.0e-4
    // there -- 0.06 sigma. At envH = 8 the same code would have failed
    // at 3.6 sigma. That is precisely the class of latency an evenness
    // assert cannot see, which is why this one names its band.
    static_assert(kEnvH >= 8u && kEnvH <= 128u,
                 "checks 29-31 are derived for env heights in [8, 128]: below 8 the "
                 "delta <= (1-kappa)*mean(X) allowance is outside every resolution it was "
                 "measured at, and above 128 the coarsest row centre's cosine approaches "
                 "the 1e-4 grazing/sin floors that check 31 compares against "
                 "unconditionally");
    static_assert(kEnvW >= kEnvH,
                 "the azimuthal resolution must not be coarser than the polar one: the "
                 "kappa derivation integrates each row exactly in phi and only quadratures "
                 "in theta");
    constexpr uint32_t kEnvTexels = kEnvW * kEnvH;

    // A floor at y = 0 seen from directly above. The camera basis is any
    // orthonormal triple with forward = -Y; the footprint at this fov
    // and height is under +/-0.6 in x and z, so a half-size of 2 leaves
    // the quad's edges nowhere near a primary ray.
    constexpr float kFloorY = 0.0f;
    constexpr float kFloorHalfSize = 2.0f;
    constexpr float kCameraHeight = 2.0f;
    constexpr float kTanHalfFov = 0.2f;

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kZ = 6.0;

    // --- The environment. Strictly positive everywhere (so every
    // returned density is positive and every recovered radiance is
    // finite), asymmetric in both axes, brighter towards the +Y pole so
    // that most of the environment's energy is in the hemisphere the
    // floor can actually see, and with a bright block ALSO in that
    // hemisphere so the two strategies genuinely disagree about where to
    // put their samples. A block in the lower hemisphere would make
    // next-event estimation spend most of its samples on directions the
    // cosine kills, which weakens the check for no reason. ---
    std::vector<float> envRgba(static_cast<std::size_t>(kEnvTexels) * 4u, 0.0f);
    std::vector<double> envLum(kEnvTexels, 0.0);
    constexpr double kBlockBoost = 8.0;
    const uint32_t kBlockY0 = static_cast<uint32_t>(kEnvH * 0.15);
    const uint32_t kBlockX0 = static_cast<uint32_t>(kEnvW * 0.60);
    const uint32_t kBlockH = kEnvH / 8u;
    const uint32_t kBlockW = kEnvW / 8u;
    for (uint32_t y = 0; y < kEnvH; ++y) {
        for (uint32_t x = 0; x < kEnvW; ++x) {
            double L = 1.0 + 0.10 * (static_cast<double>(x) * 16.0 / kEnvW) +
                       0.30 * (static_cast<double>(kEnvH - 1u - y) * 8.0 / kEnvH);
            if (y >= kBlockY0 && y < kBlockY0 + kBlockH && x >= kBlockX0 &&
                x < kBlockX0 + kBlockW) {
                L += kBlockBoost;
            }
            const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
            envLum[k] = L;
            envRgba[k * 4u + 0u] = static_cast<float>(L);
            envRgba[k * 4u + 1u] = static_cast<float>(L);
            envRgba[k * 4u + 2u] = static_cast<float>(L);
            envRgba[k * 4u + 3u] = 1.0f;
        }
    }
    static_assert(kBlockBoost > 1.0,
                 "the bright block is what makes the two sampling densities disagree; "
                 "without it MIS has nothing to combine");

    ohao::EnvCDF envCdf;
    envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    if (!envCdf.valid()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31: EnvCDF::build produced no "
                              "CDF for a %ux%u strictly-positive environment\n",
                     kEnvW, kEnvH);
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 buffers build\n");
        return false;
    }
    if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                              envCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 env CDF upload rejected\n");
        wf.destroy(ctx.allocator());
        return false;
    }

    ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

    ohao::diff::WavefrontGenerateCamera camera;
    camera.origin[0] = 0.0f;
    camera.origin[1] = kCameraHeight;
    camera.origin[2] = 0.0f;
    camera.forward[0] = 0.0f;
    camera.forward[1] = -1.0f;
    camera.forward[2] = 0.0f;
    camera.right[0] = 1.0f;
    camera.right[1] = 0.0f;
    camera.right[2] = 0.0f;
    camera.up[0] = 0.0f;
    camera.up[1] = 0.0f;
    camera.up[2] = -1.0f;
    camera.tanHalfFov = kTanHalfFov;

    std::vector<uint32_t> queue0;
    if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 setup: wf_generate\n");
        wf.destroy(ctx.allocator());
        return false;
    }

    // The floor, as ONE triangle soup handed to BOTH the primary trace
    // and the shadow rays. Passing the same span to both is the point:
    // a shadow ray tested against different geometry than the primary
    // ray is a visibility term that means nothing.
    const float e = kFloorHalfSize;
    const std::array<float, 12> floorPositions = {
        -e, kFloorY, -e,
         e, kFloorY, -e,
         e, kFloorY,  e,
        -e, kFloorY,  e,
    };
    const std::array<uint32_t, 6> floorIndices = {0, 1, 2, 0, 2, 3};

    std::vector<uint32_t> queue1;
    if (!ctx.runWavefrontIntersectOnGeometry(wf, std::span<const float>(floorPositions),
                                             std::span<const uint32_t>(floorIndices),
                                             queue1)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 setup: wf_intersect\n");
        wf.destroy(ctx.allocator());
        return false;
    }

    ohao::diff::WavefrontShadowScene shadowScene;
    shadowScene.positions = std::span<const float>(floorPositions);
    shadowScene.indices = std::span<const uint32_t>(floorIndices);

    std::vector<uint32_t> outQueue;
    std::vector<float> outDraws;
    std::vector<float> envSamples;
    std::vector<float> neeSamples;
    if (!ctx.runWavefrontScatterProbe(
            wf, /*srcQueueBase=*/kCapacity,
            ohao::diff::WavefrontBuffers::kNextCountSlot, /*dstQueueBase=*/0u,
            ohao::diff::WavefrontBuffers::kCurrentCountSlot, kAlbedo, kIterationSeed, outQueue,
            outDraws, /*material=*/{}, &envSamples, shadowScene, &neeSamples)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 scatter dispatch\n");
        wf.destroy(ctx.allocator());
        return false;
    }
    const float envIntegral = wf.envIntegral();
    wf.destroy(ctx.allocator());

    if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats ||
        neeSamples.size() !=
            static_cast<std::size_t>(kCapacity) * ohao::diff::kNeeSampleFloats) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: checks 29-31 readback returned %zu env floats "
                     "and %zu NEE floats, expected %u and %u\n",
                     envSamples.size(), neeSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats,
                     kCapacity * ohao::diff::kNeeSampleFloats);
        return false;
    }

    // --- Host-side accumulation. Deliberately NOT the GPU's: Task 4's
    // estimators are formed here from per-sample readback, which makes
    // the accumulator an oracle independent of whatever Task 5 does to
    // film accumulation, and keeps these verdicts valid across that
    // change.
    double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
    double sumD1 = 0.0, sumD2 = 0.0, sumD3 = 0.0;
    double sumD1Sq = 0.0, sumD2Sq = 0.0, sumD3Sq = 0.0;
    double sumXSq = 0.0, sumYSq = 0.0, sumZSq = 0.0;
    uint32_t contributing = 0;
    uint32_t surfaceSamples = 0;
    // 29's kappa: the closed-form midpoint/exact ratio derived above.
    const double kappa = std::sin(kPi / kEnvH) / (kPi / kEnvH);

    // 30's accumulators.
    double worstPartitionError = 0.0;
    uint32_t worstPartitionSample = 0;

    // 31's accumulators.
    double worstRadianceRelError = 0.0;
    double worstBsdfRadianceRelError = 0.0;
    double worstPdfBsdfAbsError = 0.0;
    double worstNeeTieRelError = 0.0;
    double worstPdfEnvNormalisedError = 0.0;
    double worstPdfEnvRelError = 0.0;
    uint32_t pdfEnvAtBsdfRejected = 0;
    uint32_t pdfEnvAtBsdfSkipped = 0;
    double minPdfEnvAtBsdf = std::numeric_limits<double>::infinity();

    // The host's own texel densities, computed from the luminance image
    // and EnvCDF's integral and nothing the GPU produced:
    //
    //     p(x,y) = L(x,y) * W * H / (integral * 2 pi^2)
    //
    // WHAT pdfEnvMap ACTUALLY RETURNS, which is not that. Its texel mass
    // condDiff*margDiff carries sin(theta) of the texel CENTRE (the CDF
    // is built with that weight), and it then divides by sin(theta) of
    // the QUERY direction. So
    //
    //     pdfEnvMap(w) = p(x,y) * sin(theta_centre) / sin(theta_w),
    //
    // equal to p only when w IS the texel centre -- which is exactly
    // where sampleEnvMap puts every one of its samples, so the two agree
    // on the environment strategy's entire support and the MIS weights
    // still partition unity (check 30 measures that directly). Off a
    // centre the factor can reach several: the smallest sin(theta_w) a
    // cosine-weighted sample about +Y reaches in 49152 draws is around
    // 0.005, against a first-row centre at sin(theta) = 0.0245.
    //
    // NOT A NEW FINDING, and this file should not imply it is.
    // site/content/units/sampling/env-cdf.md already says the two
    // "differ by sin(theta_y)/sin(theta(w))", that it is "worst at the
    // poles", and works a 2048-high map to "a factor of 7.7 between the
    // two sides of one weight". What IS new here is that pdfEnvMap has
    // a caller under test at all -- check 24 deliberately writes its own
    // inverse rather than calling it -- and that the factor is asserted
    // per sample rather than described.
    //
    // This check asserts the factor rather than ignoring it. Asserting
    // p alone was tried first and rejected 1019 of 49152 samples; a
    // check written to the convenient formula instead would have been
    // the weaker one. Note the direction of the lesson: the ratio is
    // harmless in a WEIGHT (it cancels) and a bias in a RADIANCE (it
    // does not), which is why wf_scatter.comp now reads BOTH densities
    // at the BSDF direction and why the radiance it recovered is
    // asserted separately below.
    const double kTwoPiSq = 2.0 * kPi * kPi;
    std::vector<double> hostTexelPdf(kEnvTexels, 0.0);
    for (uint32_t k = 0; k < kEnvTexels; ++k) {
        hostTexelPdf[k] = envLum[k] * static_cast<double>(kEnvW) *
                          static_cast<double>(kEnvH) /
                          (static_cast<double>(envIntegral) * kTwoPiSq);
    }

    // Float32 budget for the per-sample ties: the recovered radiance is
    // two CDF differences (each up to 2^-23 absolute, ~7e-6 relative on
    // the least likely texel here), a W*H product, a multiply by a
    // float32 integral and a divide -- a few parts in 1e-5. Asserted at
    // 1e-3, two orders of magnitude of slack, and the observed maxima
    // are printed so a value creeping towards the bound is visible.
    constexpr double kTieRelTolerance = 1e-3;
    // ABSOLUTE, and separate, because the quantity it bounds is not a
    // ratio. `worstPdfBsdfAbsError` is |pdfBsdfAtLight - max(0,d.y)/pi|,
    // a difference of two numbers in [0, 1/pi]; comparing it against a
    // RELATIVE constant (which is what this used to do, reusing
    // kTieRelTolerance) is a category error that happened to be
    // harmless only because the observed value is 2.3e-8. Derived
    // instead: the shader's dot(vec3(0,1,0), envDir) is exactly d.y,
    // both sides then divide the SAME float32 by pi, so the difference
    // is a couple of ulp of 1/pi = 0.3183 -- one ulp there is 3.0e-8.
    // 1e-6 is about 32 ulp: loose enough not to be a hardware lottery,
    // tight enough that a genuinely different direction (the failure it
    // exists to catch) is nowhere near it.
    constexpr double kPdfBsdfAbsTolerance = 1e-6;
    // The MIS partition is two float32 divisions by the same
    // denominator; their sum is 1 to within a couple of ulp.
    constexpr double kPartitionTolerance = 1e-6;

    for (uint32_t i = 0; i < kCapacity; ++i) {
        const std::size_t nb = static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
        if (neeSamples[nb + ohao::diff::kNeeSlotSurfaceBranch] == 0.0f) continue;
        ++surfaceSamples;

        const double X = neeSamples[nb + ohao::diff::kNeeSlotNeeUnweighted];
        const double Y = neeSamples[nb + ohao::diff::kNeeSlotBsdfUnweighted];
        const double wEnvAtLight = neeSamples[nb + ohao::diff::kNeeSlotWEnvAtLight];
        const double wBsdfAtLight = neeSamples[nb + ohao::diff::kNeeSlotWBsdfAtLight];
        const double wBsdfAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotWBsdfAtBsdf];
        const double wEnvAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotWEnvAtBsdf];
        const double Z = wEnvAtLight * X + wBsdfAtBsdf * Y;

        if (!std::isfinite(X) || !std::isfinite(Y) || X < 0.0 || Y < 0.0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 29 -- path %u reported a non-finite or "
                         "negative estimator (nee %.9g, bsdf %.9g). A radiance estimator is a "
                         "non-negative real; anything else is a division by a density the "
                         "direction was not drawn from\n",
                         i, X, Y);
            return false;
        }
        if (X > 0.0 || Y > 0.0) ++contributing;

        sumX += X; sumY += Y; sumZ += Z;
        sumXSq += X * X; sumYSq += Y * Y; sumZSq += Z * Z;
        const double d1 = Y - kappa * X;
        const double d2 = Z - Y;
        const double d3 = Z - X;
        sumD1 += d1; sumD2 += d2; sumD3 += d3;
        sumD1Sq += d1 * d1; sumD2Sq += d2 * d2; sumD3Sq += d3 * d3;

        // --- 30. Both MIS partitions, per sample.
        const double e1 = std::abs(wEnvAtLight + wBsdfAtLight - 1.0);
        const double e2 = std::abs(wBsdfAtBsdf + wEnvAtBsdf - 1.0);
        if (std::max(e1, e2) > worstPartitionError) {
            worstPartitionError = std::max(e1, e2);
            worstPartitionSample = i;
        }

        // --- 31. Ties back to what binding 6 recorded.
        const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
        const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
        const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
        // Same host-side binning as checks 25/27 and the parity
        // reference -- one function, not a fourth transcription.
        const std::size_t texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH).index;

        const double envRadiance = neeSamples[nb + ohao::diff::kNeeSlotEnvRadiance];
        const double radRelError = std::abs(envRadiance / envLum[texel] - 1.0);
        worstRadianceRelError = std::max(worstRadianceRelError, radRelError);

        // The surface normal is +Y, so the density diffBsdfSample would
        // have drawn the LIGHT sample's direction with is max(0, d.y)/pi
        // -- computable from binding 6's direction and nothing else.
        // diffBsdfEval reports 0 below its 1e-4 grazing floor; the
        // coarsest row of this map sits at cos(theta) = 0.0245, two
        // orders of magnitude above it, so that branch is unreachable
        // here and the comparison is unconditional.
        const double expectedPdfBsdf = std::max(0.0, dy) / kPi;
        const double pdfBsdfAtLight = neeSamples[nb + ohao::diff::kNeeSlotPdfBsdfAtLight];
        worstPdfBsdfAbsError =
            std::max(worstPdfBsdfAbsError, std::abs(pdfBsdfAtLight - expectedPdfBsdf));

        // And the estimator itself, recomputed from binding 6's
        // (direction, pdf) pair: f = albedo/pi, cos = max(0, d.y),
        // L = the map's own luminance at the texel that direction lands
        // in, V = 1 (nothing occludes the upper hemisphere above a
        // planar floor). If next-event estimation had drawn its own
        // direction rather than consuming the one binding 6 records,
        // this would not match.
        const double envPdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];
        const double expectedNee = (envPdf > 0.0)
                                       ? (static_cast<double>(kAlbedo) / kPi) *
                                             std::max(0.0, dy) * envLum[texel] / envPdf
                                       : 0.0;
        if (expectedNee > 0.0) {
            worstNeeTieRelError =
                std::max(worstNeeTieRelError, std::abs(X / expectedNee - 1.0));
        } else if (X != 0.0) {
            worstNeeTieRelError = std::max(worstNeeTieRelError, 1.0);
        }

        // --- pdfEnvMap, at the BSDF sample's own direction.
        const double pdfEnvAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotPdfEnvAtBsdf];
        minPdfEnvAtBsdf = std::min(minPdfEnvAtBsdf, pdfEnvAtBsdf);
        const double bx = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 0u];
        const double by = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 1u];
        const double bz = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 2u];
        const OracleEnvTexel bTexelInfo = oracleEnvTexelOf(bx, by, bz, kEnvW, kEnvH);
        const double bTheta = bTexelInfo.theta;
        const double bu = bTexelInfo.fx;
        const double bv = bTexelInfo.fy;
        // A direction landing within a thousandth of a texel of a
        // boundary may be binned into a different texel by the shader's
        // float32 arithmetic than by this double one, which would make
        // L (and therefore the expected density) come from the wrong
        // texel. Those samples are counted and excluded rather than
        // asserted; the count is reported so a scene that started
        // producing many of them would be visible.
        const double buFrac = std::abs(bu - std::floor(bu) - 0.5);
        const double bvFrac = std::abs(bv - std::floor(bv) - 0.5);
        if (buFrac > 0.5 - 1e-3 || bvFrac > 0.5 - 1e-3) {
            ++pdfEnvAtBsdfSkipped;
            continue;
        }
        const int biy = bTexelInfo.iy;
        const std::size_t bTexel = bTexelInfo.index;
        const double bThetaCentre = kPi * (static_cast<double>(biy) + 0.5) / kEnvH;
        const double bSinQuery = std::max(std::sin(bTheta), 1e-4);
        const double expectedPdfEnv =
            hostTexelPdf[bTexel] * std::sin(bThetaCentre) / bSinQuery;

        // THE RADIANCE STRATEGY B ACTUALLY MULTIPLIED IN, against the
        // environment image at the texel its own direction lands in.
        //
        // This is the check that separates the two candidate recoveries
        // and it is EXACT per sample, not statistical. Inverting
        // pdfEnvMap's answer (which is what this shader did for one
        // commit) yields L * sin(theta_centre)/sin(theta_query): equal
        // to L at a centre, and up to ~5x L in the tail of a
        // cosine-weighted draw about +Y, where the smallest
        // sin(theta_query) reached in 49152 draws is around 0.005
        // against a first-row centre at 0.0245. Averaged over an
        // estimator that also divides by the same measure it is a
        // 3.0e-4 shift in E[Y] at envH = 64 -- 0.06 of one standard
        // error, which is why check 29 could not see it -- but it is a
        // per-sample energy error of up to a factor of five, which is a
        // firefly source the moment Task 5 accumulates these
        // contributions into a film. Inverting pdfEnvMapTexel instead
        // yields L exactly, and the comparison below is then the same
        // float32 CDF round trip the light-sample radiance tie above
        // makes, at the same tolerance.
        const double bsdfRadiance = neeSamples[nb + ohao::diff::kNeeSlotBsdfRadiance];
        worstBsdfRadianceRelError = std::max(worstBsdfRadianceRelError,
                                             std::abs(bsdfRadiance / envLum[bTexel] - 1.0));
        const double pdfEnvRelError = std::abs(pdfEnvAtBsdf / expectedPdfEnv - 1.0);
        worstPdfEnvRelError = std::max(worstPdfEnvRelError, pdfEnvRelError);

        // TOLERANCE, derived from Vulkan's own precision guarantees
        // rather than from what this machine happens to produce. The
        // quantity divides by a float32 sin(acos(y)):
        //
        //  * sin() is guaranteed only to ABSOLUTE error 2^-11 inside
        //    [-pi, pi] (Vulkan, Precision and Operation of SPIR-V
        //    Instructions), so its RELATIVE error is 2^-11 / sin(theta)
        //    -- unbounded as the direction approaches the pole, which is
        //    why this term is per-sample and not a constant.
        //  * acos is specified as inherited from atan2, 4096 ULP, so
        //    theta carries relative error 4096 * 2^-24; sin's own
        //    argument error contributes cos(theta) * theta * that,
        //    again divided by sin(theta) to become relative.
        //  * The two CDF differences contribute ~2e-5 relative (the
        //    same float32 cancellation checks 25/26 budget), and the
        //    remaining float32 products/divisions a few ulp.
        //
        // The bound is loose near the pole and tight (7e-4) at the
        // sin(theta) ~ 0.7 where most cosine-weighted samples land. Both
        // the worst raw relative error and the worst error as a FRACTION
        // of its own sample's bound are printed, so hardware that beats
        // the specification by orders of magnitude -- as it does -- is
        // visible rather than hidden behind the guarantee.
        const double allowed =
            2.0e-5 + (4.8828125e-4 + std::cos(bTheta) * bTheta * 4096.0 * 5.9604645e-8) /
                         bSinQuery;
        worstPdfEnvNormalisedError =
            std::max(worstPdfEnvNormalisedError, pdfEnvRelError / allowed);
        if (!(pdfEnvRelError <= allowed)) ++pdfEnvAtBsdfRejected;
    }

    if (surfaceSamples != kCapacity) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: checks 29-31 -- only %u of %u paths took the "
                     "surface branch. Every primary ray from a camera above the floor hits "
                     "it, so the estimators below would be averaging over a set this check "
                     "did not choose\n",
                     surfaceSamples, kCapacity);
        return false;
    }

    const double n = static_cast<double>(kCapacity);
    const double meanX = sumX / n;
    const double meanY = sumY / n;
    const double meanZ = sumZ / n;
    auto stdErr = [n](double sum, double sumSq) {
        const double mean = sum / n;
        const double var = std::max(0.0, (sumSq - n * mean * mean) / (n - 1.0));
        return std::sqrt(var / n);
    };
    const double seD1 = stdErr(sumD1, sumD1Sq);
    const double seD2 = stdErr(sumD2, sumD2Sq);
    const double seD3 = stdErr(sumD3, sumD3Sq);
    const double meanD1 = sumD1 / n;
    const double meanD2 = sumD2 / n;
    const double meanD3 = sumD3 / n;
    const double systematic = (1.0 - kappa) * meanX;

    bool task4Failed = false;

    // --- 29. Strategy agreement. ---
    if (!(meanX > 0.0) || !(meanY > 0.0) || !(meanZ > 0.0) || contributing == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 29 -- the three estimators average "
                     "%.9g (next-event), %.9g (BSDF) and %.9g (MIS) over %u contributing "
                     "samples. Three estimators that are all ZERO agree perfectly and prove "
                     "nothing; this scene is an unoccluded floor under a strictly positive "
                     "environment, so every one of them must be positive\n",
                     meanX, meanY, meanZ, contributing);
        task4Failed = true;
    } else if (!(std::abs(meanD1) <= kZ * seD1) ||
               !(std::abs(meanD2) <= kZ * seD2 + systematic) ||
               !(std::abs(meanD3) <= kZ * seD3 + 2.0 * systematic)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 29 -- the three estimators of ONE "
                     "direct-lighting integral do not agree. next-event %.6f, BSDF %.6f, MIS "
                     "%.6f over %u samples.\n"
                     "  D1 = bsdf - kappa*nee : %+.6f, bound %.6f (%.2f z, z = %.1f, se "
                     "%.6f)\n"
                     "  D2 = mis  - bsdf      : %+.6f, bound %.6f (se %.6f + systematic "
                     "%.6f)\n"
                     "  D3 = mis  - nee       : %+.6f, bound %.6f (se %.6f + 2*systematic)\n"
                     "  kappa = sinc(pi/%u) = %.9f is the CLOSED-FORM midpoint/exact ratio "
                     "between the two strategies' expectations, not a fitted correction\n",
                     meanX, meanY, meanZ, kCapacity, meanD1, kZ * seD1,
                     seD1 > 0.0 ? std::abs(meanD1) / seD1 : 0.0, kZ, seD1, meanD2,
                     kZ * seD2 + systematic, seD2, systematic, meanD3,
                     kZ * seD3 + 2.0 * systematic, seD3, kEnvH, kappa);
        task4Failed = true;
    } else {
        std::printf("[diff_gpu_probe] OK: check 29 -- next-event-only (%.6f), BSDF-only "
                    "(%.6f) and their MIS combination (%.6f) estimate ONE direct-lighting "
                    "integral from %u samples and agree: |bsdf - kappa*nee| = %.6f at %.2f "
                    "z (bound %.6f, z = %.1f), |mis - bsdf| = %.6f at %.2f z (bound %.6f, "
                    "incl. derived systematic %.6f), |mis - nee| = %.6f at %.2f z (bound "
                    "%.6f). kappa = sinc(pi/%u) = %.9f, the closed-form midpoint/exact ratio. "
                    "%u of %u samples contributed\n",
                    meanX, meanY, meanZ, kCapacity, std::abs(meanD1),
                    seD1 > 0.0 ? std::abs(meanD1) / seD1 : 0.0, kZ * seD1, kZ,
                    std::abs(meanD2), seD2 > 0.0 ? std::abs(meanD2) / seD2 : 0.0,
                    kZ * seD2 + systematic, systematic, std::abs(meanD3),
                    seD3 > 0.0 ? std::abs(meanD3) / seD3 : 0.0, kZ * seD3 + 2.0 * systematic,
                    kEnvH, kappa, contributing, kCapacity);
    }

    // --- 30. The MIS partition, per sample. ---
    //
    // Exact and cheap where check 29 is statistical: at ONE direction
    // the two strategies' balance-heuristic weights are p_A/(p_A+p_B)
    // and p_B/(p_A+p_B), so they sum to 1 identically.
    //
    // WHAT IT CAN AND CANNOT SEE, stated precisely, because "an entire
    // class of weighting bug" is what this comment used to claim and it
    // is more than the identity supports. nee.glsl's diffMisTerm forms
    // wOwn = misBalance(a, b) and wOther = misBalance(b, a) from the
    // SAME pair (a, b), so wOwn + wOther = (a+b)/max(a+b, 1e-6) is
    // identically 1 in exact arithmetic WHATEVER a and b are. This
    // check is therefore a WITHIN-CALL identity, and it sees exactly
    // three things:
    //
    //   1. one of the two calls having its arguments swapped (Step 5's
    //      perturbation -- both weights then come from the same
    //      ordering and no longer complement),
    //   2. a balance heuristic on one side and a power heuristic on the
    //      other,
    //   3. the 1e-6 floor engaging, i.e. a sample where both densities
    //      are ~0 and the "weights" are not a partition at all.
    //
    // It CANNOT see the bug class that actually biases MIS: the two
    // strategies evaluating DIFFERENT environment densities at the same
    // direction. Both weights would still be formed from one pair and
    // would still sum to 1. That failure is check 29's to catch,
    // statistically, and check 31's to catch per sample.
    if (!(worstPartitionError <= kPartitionTolerance)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 30 -- the two MIS weights at one sampled "
                     "direction do not sum to 1: worst deviation %.3g at path %u (tolerance "
                     "%.3g, which is a couple of ulp of two float32 divisions by the same "
                     "denominator). The estimator is unbiased only if the weights partition "
                     "unity POINTWISE\n",
                     worstPartitionError, worstPartitionSample, kPartitionTolerance);
        task4Failed = true;
    } else {
        std::printf("[diff_gpu_probe] OK: check 30 -- for all %u samples, BOTH MIS partitions "
                    "sum to exactly 1 (w_env + w_bsdf at the light sample, and again at the "
                    "BSDF sample): worst deviation %.3g, tolerance %.3g\n",
                    kCapacity, worstPartitionError, kPartitionTolerance);
    }

    // --- 31. The three things nothing tested before this task. ---
    //
    // (a) envIntegral reaching the GPU intact. It is the entire scale of
    //     the recovered radiance, and a wrong value rescales ALL THREE
    //     estimators together -- so check 29 is blind to it by
    //     construction, and only an absolute comparison against the
    //     environment image can see it.
    // (b) pdfEnvMap, which had no caller under test anywhere: check 24
    //     deliberately writes its own inverse rather than calling it.
    // (c) The ROUTING claim this task rests on -- that next-event
    //     estimation consumes the very sample binding 6 records rather
    //     than drawing its own.
    //
    //     WHICH TIE ACTUALLY CLOSES THAT LOOP, stated exactly, because
    //     "a second draw could satisfy neither" is what this used to
    //     say and it is too strong. The pdf tie (pdfBsdfAtLight ==
    //     max(0, d.y)/pi) constrains d.y alone. So does the estimator
    //     tie: X = f * d.y * envRadiance / envPdf with envRadiance =
    //     envPdf * integral * 2*pi^2 / (W*H), so envPdf CANCELS OUT OF
    //     X entirely and X = f * d.y * integral * 2*pi^2 / (W*H) --
    //     again a constraint on d.y. A re-draw that shared the marginal
    //     row and re-drew only the conditional column would satisfy
    //     both.
    //
    //     The tie that closes the loop is the RADIANCE tie:
    //     envRadiance, recovered from envPdf alone, is compared against
    //     envLum at the texel binding 6's FULL 3-D direction bins into.
    //     That is the one identity a second draw could not satisfy --
    //     it pins the pdf and the direction to the same texel, which is
    //     to say to the same draw.
    // A sample count this check would rather not lose: if boundary
    // ambiguity ever excluded a large share, the pdfEnvMap verdict would
    // quietly be about a shrinking subset.
    constexpr double kMaxSkippedFraction = 0.02;
    const double skippedFraction =
        static_cast<double>(pdfEnvAtBsdfSkipped) / static_cast<double>(kCapacity);
    if (!(worstRadianceRelError <= kTieRelTolerance) ||
        !(worstBsdfRadianceRelError <= kTieRelTolerance) ||
        !(worstPdfBsdfAbsError <= kPdfBsdfAbsTolerance) ||
        !(worstNeeTieRelError <= kTieRelTolerance) || pdfEnvAtBsdfRejected != 0 ||
        !(skippedFraction <= kMaxSkippedFraction)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 31 --\n"
                     "  recovered radiance vs the environment image: worst relative error "
                     "%.3g (tolerance %.3g). This is the ONLY observable that depends on "
                     "ScatterPush::envIntegral, which nothing verified before this task.\n"
                     "  the BSDF strategy's OWN recovered radiance vs the environment image "
                     "at the texel its direction lands in: worst relative error %.3g (same "
                     "tolerance). This is what separates inverting pdfEnvMapTexel (which "
                     "gives L) from inverting pdfEnvMap (which gives "
                     "L*sin(theta_centre)/sin(theta_query), up to ~5x L off a centre).\n"
                     "  diffBsdfEval's pdf at the LIGHT sample vs max(0, d.y)/pi computed "
                     "from binding 6's direction: worst absolute error %.3g (absolute "
                     "tolerance %.3g). A mismatch means next-event estimation is not using "
                     "the direction binding 6 records.\n"
                     "  next-event estimator vs f*cos*L/p recomputed from binding 6: worst "
                     "relative error %.3g.\n"
                     "  pdfEnvMap vs p(texel)*sin(theta_centre)/sin(theta_query): %u of %u "
                     "samples outside their own derived bound (worst raw relative error "
                     "%.3g, worst as a fraction of its bound %.3g, min returned %.6g); %u "
                     "samples (%.3f%%) excluded as texel-boundary-ambiguous\n",
                     worstRadianceRelError, kTieRelTolerance, worstBsdfRadianceRelError,
                     worstPdfBsdfAbsError, kPdfBsdfAbsTolerance,
                     worstNeeTieRelError, pdfEnvAtBsdfRejected, kCapacity,
                     worstPdfEnvRelError, worstPdfEnvNormalisedError, minPdfEnvAtBsdf,
                     pdfEnvAtBsdfSkipped, 100.0 * skippedFraction);
        task4Failed = true;
    } else {
        std::printf("[diff_gpu_probe] OK: check 31 -- across %u samples: the radiance "
                    "recovered from the density and ScatterPush::envIntegral matches the "
                    "environment image to %.3g relative (tolerance %.3g -- envIntegral's "
                    "first end-to-end check anywhere), and the BSDF strategy's own recovered "
                    "radiance matches the image at ITS texel to %.3g relative, which is what "
                    "pins that side to L rather than to "
                    "L*sin(theta_centre)/sin(theta_query); the BSDF pdf at the light sample "
                    "matches max(0,d.y)/pi from binding 6's OWN direction to %.3g absolute "
                    "(tolerance %.3g) and the next-event estimator matches f*cos*L/p "
                    "recomputed from binding 6's (direction, pdf) to %.3g relative, and the "
                    "radiance tie binds that pdf to that direction, so NEE consumes the "
                    "sample check 24 bins rather than a second draw; and pdfEnvMap (min "
                    "%.6g) matches p(texel)*sin(theta_centre)/sin(theta_query) for every one "
                    "of its first tested calls -- worst raw relative error %.3g, worst %.3g "
                    "of its own Vulkan-precision-derived bound (%u samples, %.3f%%, excluded "
                    "as texel-boundary-ambiguous)\n",
                    kCapacity, worstRadianceRelError, kTieRelTolerance,
                    worstBsdfRadianceRelError, worstPdfBsdfAbsError, kPdfBsdfAbsTolerance,
                    worstNeeTieRelError, minPdfEnvAtBsdf, worstPdfEnvRelError,
                    worstPdfEnvNormalisedError, pdfEnvAtBsdfSkipped, 100.0 * skippedFraction);
    }

    if (task4Failed) return false;
    return true;
}

bool checkFilmAccumulation(ohao::diff::GpuProbeContext& ctx) {
    // ------------------------------------------------------------------
    // 32. RADIANCE ACCUMULATION INTO THE FILM (Stage 0b-2b Task 5).
    // ------------------------------------------------------------------
    //
    // wf_scatter.comp now adds
    //
    //     T_k * (w_E,k * E_k + w_B,k * B_k)
    //
    // into film[pixelIndex] by atomicAdd on EVERY bounce -- T_k the path's
    // throughput on arrival at bounce k's vertex, E_k/B_k the two
    // single-strategy direct-lighting estimators and w_E/w_B their MIS
    // weights. This check asserts that what ends up in that buffer after a
    // FUSED B-bounce run really is the sum of the B per-bounce
    // contributions, reconstructed on the host from primitives read back
    // INDEPENDENTLY of the accumulator.
    //
    // WHAT MAKES THE ORACLE INDEPENDENT. The film is not compared against a
    // copy of itself. wf_scatter.comp records T_k, E_k, w_E, B_k, w_B and
    // the pixel index as SEPARATE floats in the binding-7 sink; the host
    // multiplies and sums them in double. So this rejects an accumulator
    // that drops a bounce, that overwrites instead of adding, that resets,
    // that double-counts, that applies the post-decay throughput instead of
    // the arrival one, that uses one strategy instead of the MIS
    // combination, or that lands in the wrong pixel. It deliberately does
    // NOT re-derive E_k and B_k from the BSDF and the environment -- checks
    // 29-31 own the estimators, and Task 4's report records why those stay
    // host-accumulated and independent of this buffer.
    //
    // WHY "APPLIES THE POST-DECAY THROUGHPUT" IS ACTUALLY REJECTED. The
    // other failure modes above are backed by construction: an overwrite,
    // reset, drop, double-count, wrong-pixel or single-strategy edit changes
    // the RELATIONSHIP between what the sink records and what the film
    // holds, so the reconstruction stops matching. A post-decay-throughput
    // bug is different in kind -- wf_scatter.comp's sink write (slots 21-23)
    // and its film term read the SAME local variable (`arrivalThroughput`),
    // so an edit that moved that variable's value from the arrival
    // throughput to the post-decay one would move the sink and the film
    // TOGETHER, and a check that only compared them against each other could
    // not see it move. What actually closes that seam is the per-path
    // assertion below pinning slots 21-23 to kAlbedo^k -- a value computed
    // from nothing in the shader at all. It is sound only because checks
    // 14/17 already establish, bit-exactly, that this pure-Lambert scene's
    // per-bounce estimator weight is exactly `albedo`, which is what lets
    // "arrival throughput at bounce k" reduce to a closed-form constant
    // instead of another shader-derived quantity.
    //
    // WHERE THE PER-BOUNCE VALUES COME FROM. The binding-7 sink is written
    // at a fixed per-path offset every bounce, so only the LAST bounce's
    // record survives one fused run. runWavefrontFusedLoopProbe already runs
    // the loop once per bounce count (1, 2, ... B) for exactly this reason,
    // so the run of k+1 bounces exposes bounce k. Every run restarts from
    // zeroed buffers with the same seed, wf_scatter.comp rebuilds its RNG
    // from (pixelIndex, sampleIndex, iterationSeed, storedBounce) rather
    // than carrying it, and every stage indexes path state by path index --
    // so bounce k is bit-identical across runs. That is not assumed here: if
    // it did not hold, the sums below would not match the films, and this
    // check is what would say so.
    //
    // THE SCENE IS A RIG, NOT A SCENE. It runs the closed box (so every path
    // is alive at every bounce and every bounce therefore contributes) with
    // wf_scatter.comp's shadow rays pointed at a DIFFERENT, empty
    // acceleration structure, so those contributions are nonzero. Inside the
    // closed box every direct-lighting contribution is exactly zero -- that
    // is check 28 -- and a film check there would be comparing 0 against 0
    // and could not fail. Nothing about the estimator is concluded from this
    // run; see runWavefrontFusedLoopProbe's `unoccludedShadowRays` doc.
    //
    // WHAT THE FILM DOES NOT CONTAIN (informational; read this before Task 6
    // compares it against a PathTracer image). This check, like the shader,
    // sums ONLY MIS direct lighting at surface vertices. There is no escape
    // term: wf_intersect.comp compacts only survivors, so a path that misses
    // everything is dropped from the next bounce's queue and contributes
    // nothing further. There is, as of Stage 1 Task 4, a uniform
    // emissive-surface term gated by `pc.emission` -- but this check leaves
    // it at its 0.0 default, like every check that predates Task 4, so it
    // is still absent from THIS film by construction, not by pipeline
    // limitation (see check 42, which exercises the nonzero case). A
    // PathTracer parity comparison will therefore differ from this film by
    // exactly the directly-visible environment plus any PER-OBJECT
    // emissive term a real scene's materials carry (this subsystem still
    // has no per-object material id) -- see the doc on wf_scatter.comp's
    // binding-9 Film declaration for the full argument.
    // The closed-box rig above makes that gap unobservable from inside this
    // check, which is precisely why it has to be written down here instead.
    //
    // THE BOUND, DERIVED. Let C_k(p,c) be the exact (real-arithmetic)
    // contribution of bounce k to pixel p, channel c, formed from the
    // float32 values the sink recorded. Every factor is NONNEGATIVE
    // (throughput, f*cos, radiance, visibility in {0,1}, balance-heuristic
    // weights in [0,1]), so there is no cancellation anywhere and the
    // classic Higham bound |fl(x) - x| <= gamma_n |x| applies with
    // gamma_n = n*u/(1 - n*u), u = 2^-24 the float32 unit roundoff.
    //
    //   * The shader evaluates T*(w_E*E + w_B*B) per channel. As written
    //     that is 4 roundings (two products, one sum, one product); a
    //     compiler that distributes it to T*w_E*E + T*w_B*B uses 5. Take the
    //     larger: n_eval = 5. (FMA contraction can only reduce the error, so
    //     it stays inside this bound.)
    //   * The GPU then accumulates k such values into a zeroed float32 film
    //     by atomicAdd. 0 + C_0 is exact, so that is k-1 roundings:
    //     n_sum = k - 1.
    //   * The host recomputes the same expression in float64 from the same
    //     float32 inputs; at u_64 = 2^-53 its own error is 2^29 times
    //     smaller and is neglected.
    //
    // Total n = n_eval + n_sum = k + 4, giving relTol(k) = gamma_{k+4}.
    // At k = 4 bounces that is 8u/(1-8u) = 4.77e-7 relative.
    //
    // Plus an absolute floor of 1e-30, which exists only for the underflow
    // corner: a contribution below the float32 subnormal threshold (1.2e-38)
    // can flush to zero on the GPU while the double reconstruction keeps it,
    // and a purely relative bound would call that a 100% error. It is thirty
    // orders of magnitude below the contributions this scene actually
    // produces (~1e-2), so it cannot absorb a real discrepancy. The
    // non-vacuity gate below states that margin as a number rather than
    // leaving it to this comment.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
    constexpr uint32_t kCapacity = kW * kH;  // 512
    constexpr uint32_t kPixels = kW * kH;    // one sample per pixel here
    constexpr uint32_t kBounces = 4;
    constexpr float kAlbedo = 0.5f;
    constexpr uint32_t kIterationSeed = 20260829u;
    // Non-square, for the same reason check 27 is: it costs nothing and
    // a W<->H swap anywhere in the film path would stop being symmetric.
    constexpr uint32_t kEnvW = 16;
    constexpr uint32_t kEnvH = 4;

    constexpr double kUnitRoundoff = 1.0 / 16777216.0;  // 2^-24
    constexpr double kAbsFloor = 1e-30;
    // How many times larger than the tolerance the smallest single
    // bounce's total contribution must be for this check to be able to
    // reject a dropped bounce. 1e3 is not a physical constant -- it is
    // the margin this check refuses to run below, so that "it passed"
    // is never compatible with "there was nothing to detect".
    constexpr double kMinDiscriminationMargin = 1.0e3;

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 32 buffers build\n");
        return false;
    }

    std::vector<std::vector<float>> drawsPerBounce;
    std::vector<uint32_t> liveCountPerRun;
    std::vector<uint32_t> finalQueue;
    std::vector<std::vector<float>> neePerRun;
    std::vector<std::vector<float>> filmPerRun;
    if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                        drawsPerBounce, liveCountPerRun, finalQueue,
                                        /*outEnvSamples=*/nullptr,
                                        /*outNeeSamples=*/nullptr,
                                        /*unoccludedShadowRays=*/true, &neePerRun,
                                        &filmPerRun)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 32 fused loop dispatch\n");
        wf.destroy(ctx.allocator());
        return false;
    }
    wf.destroy(ctx.allocator());

    if (neePerRun.size() != kBounces || filmPerRun.size() != kBounces) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 32 got %zu NEE runs and %zu film runs, "
                     "expected %u of each\n",
                     neePerRun.size(), filmPerRun.size(), kBounces);
        return false;
    }

    // Running sum of the reconstructed contributions, in double.
    std::vector<double> hostFilm(static_cast<std::size_t>(kPixels) * 3u, 0.0);
    // Per-bounce total over all pixels and channels, for the
    // discrimination margin below.
    double bounceTotal[kBounces] = {};
    double worstRelError = 0.0;
    double worstAbsError = 0.0;
    uint32_t worstBounce = 0;
    uint32_t litLightSamples = 0;
    uint32_t litBsdfSamples = 0;

    for (uint32_t k = 0; k < kBounces; ++k) {
        if (neePerRun[k].size() != static_cast<std::size_t>(kCapacity) *
                                       ohao::diff::kNeeSampleFloats ||
            filmPerRun[k].size() != static_cast<std::size_t>(kPixels) * 3u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 bounce %u readback sizes are %zu "
                         "NEE floats and %zu film floats, expected %u and %u\n",
                         k, neePerRun[k].size(), filmPerRun[k].size(),
                         kCapacity * ohao::diff::kNeeSampleFloats, kPixels * 3u);
            return false;
        }
        if (liveCountPerRun[k] != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 run of %u bounces left %u live "
                         "paths, expected all %u. Every bounce must contribute for the "
                         "accumulation across bounces to be what is under test\n",
                         k + 1u, liveCountPerRun[k], kCapacity);
            return false;
        }

        // Review Finding 1 (Task 5): pin the sink's arrival-throughput
        // slots (21-23) to a constant computed independently of
        // wf_scatter.comp's own `arrivalThroughput` variable, rather
        // than letting them stand in for it unchecked. This is a pure
        // Lambert material (specularWeight = 0, metallic = 0) with
        // kAlbedo = 0.5, and checks 14/17 already establish -- bit-
        // exactly, not approximately -- that this shader's per-bounce
        // estimator weight f*cos/pdf is exactly `albedo`. So the
        // throughput arriving at bounce k (0-based: k=0 is the first
        // bounce, where the path has undergone zero decays yet) is
        // exactly kAlbedo^k. kAlbedo = 0.5 is a dyadic rational, so
        // every one of these powers -- 1, 0.5, 0.25, 0.125 for the
        // k in {0,1,2,3} this check reaches -- is exactly representable
        // in float32 AND float64 with no rounding at any step; computed
        // here by repeated multiplication (not std::pow, whose result
        // for a non-trivial exponent is not guaranteed bit-exact) so the
        // comparison below is a bit-exact `!=`, not a tolerance.
        //
        // WHY THIS CLOSES A SEAM. Without it, slots 21-23 were checked
        // only for internal consistency with the film's own
        // accumulation (both read wf_scatter.comp's `arrivalThroughput`
        // local): a shader edit that moved the sink write and the film
        // term onto the SAME wrong value (e.g. the post-decay
        // throughput instead of the arrival one) would move both
        // together and this check would still pass. Comparing slots
        // 21-23 against a value that does not come from the shader at
        // all is what makes that edit detectable. See this task's
        // report for a demonstration: recording the post-decay
        // throughput here fails this exact assertion.
        double expectedArrivalThroughput = 1.0;
        for (uint32_t decays = 0; decays < k; ++decays) {
            expectedArrivalThroughput *= static_cast<double>(kAlbedo);
        }

        // Fold bounce k's per-path contributions into hostFilm, and
        // check that every pixel is written exactly once (the film is
        // indexed by PIXEL; the mapping is read from the sink rather
        // than assumed to be the identity).
        std::vector<uint32_t> pixelHits(kPixels, 0);
        for (uint32_t i = 0; i < kCapacity; ++i) {
            const std::size_t b =
                static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
            if (neePerRun[k][b + ohao::diff::kNeeSlotSurfaceBranch] != 1.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 path %u did not take "
                             "wf_scatter.comp's surface branch at bounce %u. Inside a closed "
                             "box every ray hits a face, so a miss means the scene is not "
                             "what this check assumes and the contributions it sums would "
                             "be silently short\n",
                             i, k);
                return false;
            }
            const float pixF = neePerRun[k][b + ohao::diff::kNeeSlotPixelIndex];
            if (!(pixF >= 0.0f) || pixF >= static_cast<float>(kPixels) ||
                pixF != std::floor(pixF)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 path %u reported pixel index "
                             "%.9g at bounce %u, which is not an integer in [0, %u)\n",
                             i, static_cast<double>(pixF), k, kPixels);
                return false;
            }
            const uint32_t pix = static_cast<uint32_t>(pixF);
            ++pixelHits[pix];

            if (neePerRun[k][b + ohao::diff::kNeeSlotVisLight] != 0.0f) ++litLightSamples;
            if (neePerRun[k][b + ohao::diff::kNeeSlotVisBsdf] != 0.0f) ++litBsdfSamples;

            const double wEnv = neePerRun[k][b + ohao::diff::kNeeSlotWEnvAtLight];
            const double wBsdf = neePerRun[k][b + ohao::diff::kNeeSlotWBsdfAtBsdf];
            for (uint32_t c = 0; c < 3u; ++c) {
                const double thr =
                    neePerRun[k][b + ohao::diff::kNeeSlotArrivalThroughput + c];
                if (thr != expectedArrivalThroughput) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 path %u channel %u at "
                                 "bounce %u recorded arrival throughput %.17g at binding-7 "
                                 "slot %u, but this pure-Lambert scene's arrival throughput "
                                 "at bounce %u is exactly kAlbedo^%u = %.17g, bit-exactly "
                                 "(checks 14/17 already establish the per-bounce estimator "
                                 "weight is exactly `albedo`). This pins slots 21-23 to a "
                                 "value independent of wf_scatter.comp's own "
                                 "`arrivalThroughput` variable -- see this check's header, "
                                 "'the bolded claim', and the task-5 fix report's "
                                 "demonstration: recording the POST-decay throughput here "
                                 "instead of the arrival one fails this exact assertion\n",
                                 i, c, k, thr,
                                 static_cast<unsigned>(ohao::diff::kNeeSlotArrivalThroughput) +
                                     c,
                                 k, k, expectedArrivalThroughput);
                    return false;
                }
                const double nee = neePerRun[k][b + ohao::diff::kNeeSlotNeeUnweighted + c];
                const double bsdf = neePerRun[k][b + ohao::diff::kNeeSlotBsdfUnweighted + c];
                const double contribution = thr * (wEnv * nee + wBsdf * bsdf);
                hostFilm[static_cast<std::size_t>(pix) * 3u + c] += contribution;
                bounceTotal[k] += contribution;
            }
        }
        // STRUCTURALLY HARD-CODED TO 1 SAMPLE PER PIXEL (review Finding
        // 4, Task 6 will want more). `pixelHits[p] != 1u` demands
        // exactly one path per pixel; it cannot be re-pointed at >1 spp
        // without editing this loop (and kCapacity/kPixels above, which
        // are tied 1:1 here). That is a documented constraint for
        // whoever wires more samples in, not a bug in this check as
        // written for 1 spp.
        //
        // A second fact matters for that future work and does NOT show
        // up as a failure today: at >1 spp, multiple paths sharing a
        // pixel hit `atomicAdd(counters...)` for the SAME destination
        // slot, so which path's record ends up at which queue slot
        // becomes an intra-dispatch race -- nondeterministic run to run.
        // That weakens (does not break) the cross-run bit-identity
        // runWavefrontFusedLoopProbe's bounce-by-bounce reconstruction
        // rests on: it only requires bounce k to be bit-identical FOR A
        // GIVEN PATH across runs, and per-path RNG reconstruction from
        // (pixelIndex, sampleIndex, iterationSeed, bounce) still gives
        // that regardless of queue order -- so the reconstruction itself
        // is fine, but a check written the way this one is (indexing by
        // PATH, then reading which pixel it landed on) would need to sum
        // per-pixel across paths rather than assume a 1:1 path<->pixel
        // map. The gamma_{k+4} bound is unaffected either way: it is
        // derived for a nonnegative sum and is order-independent by
        // construction.
        for (uint32_t p = 0; p < kPixels; ++p) {
            if (pixelHits[p] != 1u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 pixel %u was written by %u "
                             "paths at bounce %u, expected exactly 1 (this probe runs one "
                             "sample per pixel)\n",
                             p, pixelHits[p], k);
                return false;
            }
        }

        // gamma_{k+4} -- see the derivation in this check's header. k is
        // 0-based here, so the run summed k+1 bounces and n = (k+1) + 4.
        const double n = static_cast<double>(k + 1u) + 4.0;
        const double relTol = (n * kUnitRoundoff) / (1.0 - n * kUnitRoundoff);
        for (std::size_t idx = 0; idx < hostFilm.size(); ++idx) {
            const double gpu = filmPerRun[k][idx];
            const double host = hostFilm[idx];
            const double absErr = std::abs(gpu - host);
            const double allowed = relTol * std::abs(host) + kAbsFloor;
            if (!(absErr <= allowed) || !std::isfinite(gpu)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 -- after a FUSED run of %u "
                             "bounces, film[pixel %zu][channel %zu] = %.9g but the sum of "
                             "the %u per-bounce contributions, reconstructed on the host "
                             "from independently recorded throughput/estimator/MIS-weight "
                             "primitives, is %.9g. Absolute error %.3g, allowed %.3g "
                             "(gamma_%g = %.3g relative, plus a %.3g absolute floor)\n",
                             k + 1u, idx / 3u, idx % 3u, gpu, k + 1u, host, absErr, allowed,
                             n, relTol, kAbsFloor);
                return false;
            }
            worstAbsError = std::max(worstAbsError, absErr);
            if (std::abs(host) > 0.0) {
                const double rel = absErr / std::abs(host);
                if (rel > worstRelError) {
                    worstRelError = rel;
                    worstBounce = k + 1u;
                }
            }
        }
    }

    // --- NON-VACUITY. All of the above is satisfied by a film of zeros
    // and a sink of zeros. These are the gates that say there was
    // something to detect.
    double finalTotal = 0.0;
    for (const double v : hostFilm) finalTotal += v;
    double minBounceTotal = bounceTotal[0];
    double maxBounceTotal = bounceTotal[0];
    for (uint32_t k = 0; k < kBounces; ++k) {
        minBounceTotal = std::min(minBounceTotal, bounceTotal[k]);
        maxBounceTotal = std::max(maxBounceTotal, bounceTotal[k]);
    }
    if (litLightSamples == 0 || litBsdfSamples == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 32 -- %u light-sample and %u BSDF-sample "
                     "shadow rays were unoccluded across %u bounces. This run points the "
                     "shadow rays at an EMPTY acceleration structure precisely so they are "
                     "not occluded; with all of them blocked every contribution is zero and "
                     "the accumulation check would compare 0 against 0\n",
                     litLightSamples, litBsdfSamples, kBounces);
        return false;
    }
    const double finalRelTol =
        ((kBounces + 4.0) * kUnitRoundoff) / (1.0 - (kBounces + 4.0) * kUnitRoundoff);
    if (!(minBounceTotal > kMinDiscriminationMargin * finalRelTol * finalTotal) ||
        !(finalTotal > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 32 -- the smallest single bounce "
                     "contributes %.9g of a film total of %.9g. Dropping that bounce "
                     "entirely would have to move the film by more than %.0fx the "
                     "tolerance (%.3g relative) for this check to be able to reject it, and "
                     "it does not -- so a pass here would not be evidence\n",
                     minBounceTotal, finalTotal, kMinDiscriminationMargin, finalRelTol);
        return false;
    }

    std::printf("[diff_gpu_probe] OK: check 32 -- after a FUSED %u-bounce run through "
                "WavefrontLoop::record, the caller-owned film buffer holds exactly the sum "
                "of the %u per-bounce contributions reconstructed on the host from "
                "independently recorded primitives (arrival throughput, both "
                "single-strategy estimators, both MIS weights, the pixel index): worst "
                "relative error %.3g over %u pixels x 3 channels x %u prefix lengths (worst "
                "at %u bounces; bound gamma_{k+4}, %.3g at k=%u), worst absolute error "
                "%.3g. Non-vacuous: the film total is %.6g, the smallest single bounce "
                "contributes %.6g of it (the largest is %.1fx that, and the smallest is "
                "%.3g times the tolerance -- floor %.0fx), and %u light-sample / %u "
                "BSDF-sample shadow rays reached the environment\n",
                kBounces, kBounces, worstRelError, kPixels, kBounces, worstBounce,
                finalRelTol, kBounces, worstAbsError, finalTotal, minBounceTotal,
                (minBounceTotal > 0.0 ? maxBounceTotal / minBounceTotal : 0.0),
                minBounceTotal / (finalRelTol * finalTotal), kMinDiscriminationMargin,
                litLightSamples, litBsdfSamples);
    return true;
}

}  // namespace ohao::diff::probe
