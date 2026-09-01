// Environment checks 24-28: importance sampling against an independent
// oracle, the pdf identities, the production push-constant fill, and the
// shadow ray.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_env.hpp"

#include "probe/oracle_bsdf.hpp"

#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkEnvImportanceSampling(ohao::diff::GpuProbeContext& ctx) {
    // 24-26. ENVIRONMENT IMPORTANCE SAMPLING (Stage 0b-2b Task 3).
    //
    // wf_scatter.comp now draws a direction from the environment's
    // sin(theta)-weighted luminance every bounce, through
    // shaders/includes/rt/env_sampling.glsl's sampleEnvMap -- the same header
    // the RT pipeline's raygen shaders call -- and writes the (direction,
    // pdf) pair to its binding-6 sink. Nothing consumes it yet (NEE and MIS
    // are Task 4), so it is checked directly rather than through an image.
    //
    // WHAT IS AND IS NOT UNDER TEST. The CDF arrays uploaded to the GPU are
    // built by ohao::EnvCDF (ohao/render/rt/env_cdf.cpp), the SAME builder
    // that feeds the RT pipeline. Under test are: the GPU's two binary
    // searches over those arrays, the texel -> direction map, the CDF ->
    // solid-angle pdf conversion, and the binding/push-constant path that
    // gets the arrays to the shader THROUGH THIS PROBE'S OWN CALL SITE --
    // GpuProbeContext::runWavefrontScatterProbe, which fills ScatterPush's
    // envWidth/envHeight/envIntegral BY HAND (gpu_probe_context.cpp). It is
    // NOT a test of ohao::diff::WavefrontLoop::record's OWN fill of those
    // same three fields at its own, separate call site
    // (wavefront_loop.cpp) -- record() is what the production wavefront
    // loop actually calls, and until check 27 below, nothing exercised it
    // with an environment where a mistake in that fill would be visible.
    //
    // THE ORACLE IS NOT THE CDF. It would have been easy to bin the samples
    // and compare against differences of the uploaded CDF arrays, but that
    // oracle shares EnvCDF's own normalisation with the thing under test: a
    // builder that (say) forgot the sin(theta) weight would produce a CDF the
    // GPU sampled faithfully and the check would pass. The expected texel
    // probabilities below are instead computed here, in double precision,
    // straight from the luminance image and the analytic solid-angle weight:
    //
    //     p(x,y) = L(x,y) sin(theta_y) / sum over all texels of the same,
    //     theta_y = pi (y + 0.5) / H,
    //
    // which is the distribution the whole pipeline -- builder included -- is
    // SUPPOSED to realise. That makes EnvCDF part of what check 24 tests, not
    // part of its oracle.
    //
    // ------------------------------------------------------------------
    // 24. The chi-squared bound, derived
    // ------------------------------------------------------------------
    //
    // N samples are drawn and binned into the K = envW * envH texels of the
    // map. Under the null hypothesis (the sampler realises p exactly, and the
    // samples are independent), the count vector is multinomial(N, p) and
    //
    //     X^2 = sum over k of (O_k - E_k)^2 / E_k,   E_k = N p_k
    //
    // converges to chi-squared with K - 1 degrees of freedom (one constraint:
    // the counts sum to N). Pearson's approximation is conventionally taken
    // as adequate once every E_k >= 5; that condition is ASSERTED below
    // rather than assumed, and the observed minimum is printed, so a later
    // change to the environment or the sample count that quietly invalidates
    // the approximation fails loudly instead of weakening the test.
    //
    // The rejection threshold is the (1 - alpha) quantile of chi-squared with
    // K - 1 = 127 degrees of freedom at alpha = 1e-6. alpha is that small on
    // purpose: this probe is deterministic (fixed seeds, fixed geometry), so
    // there is no run-to-run flake to trade against, and the failures worth
    // catching -- a wrong search, a wrong row stride, a wrong pdf -- move X^2
    // by orders of magnitude, not by a factor of two. Choosing 1e-6 over,
    // say, 0.01 costs almost no power against those and removes any argument
    // that a pass was luck.
    //
    // The quantile is computed, not tabulated, via the Wilson-Hilferty
    // transform (Wilson & Hilferty, PNAS 17 (1931) 684): (X^2/df)^(1/3) is
    // approximately normal with mean 1 - 2/(9 df) and variance 2/(9 df), so
    //
    //     chi2_{df, 1-alpha} ~= df * (1 - 2/(9df) + z_alpha sqrt(2/(9df)))^3
    //
    // with z_alpha = 4.753424 the standard normal 1 - 1e-6 quantile. At
    // df = 127 that gives ~217.9. The transform's accuracy at this df was
    // checked against a published table at a quantile tables actually carry:
    // it returns 149.49 for chi2_{100, 0.999} against the tabulated 149.449,
    // an error of 0.03% -- three orders of magnitude smaller than the margin
    // between a passing and a failing run here.
    //
    // NOTHING BELOW WAS TUNED. The bound was derived first; the observed X^2
    // is printed on the OK: line so that a value creeping up toward it is
    // visible rather than silent.
    //
    // ------------------------------------------------------------------
    // 26. Why the pdfs must sum to exactly 1, and to what tolerance
    // ------------------------------------------------------------------
    //
    // sampleEnvMap returns pdfUV / (2 pi^2 sin theta) with
    // pdfUV = condDiff * margDiff * W * H. The midpoint solid angle of texel
    // (x,y) is dOmega = (2pi/W)(pi/H) sin theta_y with the SAME theta_y, so
    //
    //     pdf(x,y) * dOmega(x,y) = condDiff * margDiff = p_CDF(x,y),
    //
    // and summing over every texel must give exactly 1 -- the sine cancels,
    // leaving the CDF's own total mass. This is an identity, not a quadrature
    // approximation, so the only error is float32 arithmetic:
    //
    //   * condDiff and margDiff are differences of CDF entries near 1, each
    //     with absolute error at most one ulp of 1.0f (2^-24), so at most
    //     2^-23 = 1.19e-7 per difference. Their contribution to the total is
    //     1.19e-7 * (sum over texels of margDiff + sum over texels of
    //     condDiff) = 1.19e-7 * (W * 1 + H * 1) = 1.19e-7 * 24 = 2.9e-6.
    //   * The remaining float32 operations (the W*H product, the division by
    //     2 pi^2 sin theta, storing the result) contribute a few ulp of
    //     RELATIVE error each; against a total of 1 that is under 1e-6.
    //   * The GPU divides by a float32 sin(theta) and the host multiplies by
    //     a double sin(theta); the two differ by ~1e-7 relative, under 1e-7
    //     against a probability-weighted total of 1.
    //
    // Total: about 4e-6. The asserted tolerance is 1e-5, roughly 2.5x that,
    // and the observed deviation is printed.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;
    constexpr uint32_t kCapacity = kW * kH;  // 3072
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;
    constexpr uint32_t kIterationSeed = 909090u;

    constexpr uint32_t kEnvW = 16;
    constexpr uint32_t kEnvH = 8;
    constexpr uint32_t kEnvTexels = kEnvW * kEnvH;  // 128 bins, df = 127
    // Eight scatter dispatches over the same 3072 paths. The bounce
    // counter advances with each, so wf_scatter.comp's fast-forward puts
    // every dispatch at a different position in each path's stream and
    // the 8 * 3072 = 24576 draws are 24576 DIFFERENT PCG outputs, not
    // eight repeats. Keeping one iterationSeed throughout is deliberate:
    // that is the configuration the integrator actually runs in, so the
    // stream this check exercises is the production one.
    constexpr uint32_t kEnvDispatches = 8;
    constexpr uint32_t kSampleCount = kCapacity * kEnvDispatches;

    // Pearson's rule of thumb, asserted rather than assumed.
    constexpr double kMinExpectedPerBin = 5.0;
    // Standard normal 1 - 1e-6 quantile, for the Wilson-Hilferty
    // transform above.
    constexpr double kChiSqZ = 4.753424;
    constexpr double kChiSqAlpha = 1e-6;
    // Derived above: the identity's float32 error budget is ~4e-6.
    constexpr double kPdfSumTolerance = 1e-5;
    // Texel-centre round-trip slack. equirectPixelToDir emits the CENTRE
    // of the chosen texel, so inverting it must land within half a texel
    // of an integer + 0.5. The float32 error in that round trip is under
    // 2e-6 (acos is worst at the polar rows, where its derivative is
    // 1/sin(theta) = 5.13 at H = 8); 1e-3 is three orders of magnitude
    // of slack and still 500x tighter than the half-texel that would
    // make the binning ambiguous.
    constexpr double kCentreSlack = 1e-3;

    constexpr double kPi = 3.14159265358979323846;

    // --- The environment. Strictly positive in every texel (so every
    // bin has non-zero expected probability and every returned pdf must
    // be > 0), asymmetric in BOTH axes (so a transposed or reversed
    // index cannot coincide with the truth), and with a small bright
    // block so that importance sampling has something to concentrate on
    // and a uniform sampler is not accidentally close. ---
    std::vector<float> envRgba(static_cast<std::size_t>(kEnvTexels) * 4u, 0.0f);
    std::vector<double> envLum(kEnvTexels, 0.0);
    for (uint32_t y = 0; y < kEnvH; ++y) {
        for (uint32_t x = 0; x < kEnvW; ++x) {
            double L = 1.0 + 0.1 * static_cast<double>(x) + 0.3 * static_cast<double>(y);
            if (x >= 10 && x <= 11 && y >= 2 && y <= 3) L += 20.0;
            const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
            envLum[k] = L;
            envRgba[k * 4u + 0u] = static_cast<float>(L);
            envRgba[k * 4u + 1u] = static_cast<float>(L);
            envRgba[k * 4u + 2u] = static_cast<float>(L);
            envRgba[k * 4u + 3u] = 1.0f;
        }
    }

    // --- The oracle: p(x,y) proportional to L * sin(theta), in double,
    // from the luminance image above and nothing else. EnvCDF's grey
    // response (0.2126 + 0.7152 + 0.0722 = 1) is applied here too so the
    // two agree on the scalar being distributed, but the normalisation,
    // the sine weight and the row/column structure are all recomputed
    // independently of the CDF that was uploaded. ---
    std::vector<double> expectedP(kEnvTexels, 0.0);
    std::vector<double> sinThetaRow(kEnvH, 0.0);
    double envTotal = 0.0;
    for (uint32_t y = 0; y < kEnvH; ++y) {
        const double theta = kPi * (static_cast<double>(y) + 0.5) / static_cast<double>(kEnvH);
        sinThetaRow[y] = std::sin(theta);
        for (uint32_t x = 0; x < kEnvW; ++x) {
            const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
            const double grey = (0.2126 + 0.7152 + 0.0722) * envLum[k];
            expectedP[k] = grey * sinThetaRow[y];
            envTotal += expectedP[k];
        }
    }
    for (double& p : expectedP) p /= envTotal;

    double minExpectedCount = 1e300;
    for (uint32_t k = 0; k < kEnvTexels; ++k) {
        minExpectedCount = std::min(minExpectedCount,
                                    expectedP[k] * static_cast<double>(kSampleCount));
    }
    if (!(minExpectedCount >= kMinExpectedPerBin)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: env chi-squared setup: the least likely of the "
                     "%u texels has expected count %.4g, below the %.1f Pearson's "
                     "approximation needs. The chi-squared distribution would not be the "
                     "right reference distribution for this test -- raise kEnvDispatches or "
                     "reduce the environment's dynamic range rather than lowering this\n",
                     kEnvTexels, minExpectedCount, kMinExpectedPerBin);
        return false;
    }

    // --- The CDF actually uploaded: ohao::EnvCDF, the RT pipeline's own
    // builder, so the diff pipeline cannot drift from it. ---
    ohao::EnvCDF envCdf;
    envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    if (!envCdf.valid()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: ohao::EnvCDF::build produced no CDF for "
                              "a %ux%u strictly-positive environment\n",
                     kEnvW, kEnvH);
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling buffers build\n");
        return false;
    }
    if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                              envCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: env CDF upload rejected (%zu marginal, "
                              "%zu conditional floats for %ux%u)\n",
                     envCdf.marginalCDF().size(), envCdf.conditionalCDF().size(), kEnvW,
                     kEnvH);
        wf.destroy(ctx.allocator());
        return false;
    }

    ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

    ohao::diff::WavefrontGenerateCamera camera;
    camera.tanHalfFov = kTanHalfFov;
    std::vector<uint32_t> queue0;
    if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling setup: wf_generate\n");
        wf.destroy(ctx.allocator());
        return false;
    }
    // One trace, so every path has a real hit point and a real geometric
    // normal and the scatter dispatches below take their surface branch
    // rather than the miss guard. The environment sample itself does not
    // depend on either -- it is drawn before the guard -- but running the
    // stage in its degenerate configuration would be a weaker test of the
    // dispatch as a whole.
    std::vector<uint32_t> queue1;
    if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling setup: wf_intersect\n");
        wf.destroy(ctx.allocator());
        return false;
    }

    std::vector<uint32_t> binCount(kEnvTexels, 0u);
    // The pdf the GPU reported for each texel, and whether it reported
    // one at all. Every sample of the same texel must return the SAME
    // pdf bit for bit: it is a pure function of the two CDF arrays and
    // the texel index, evaluated by the same instructions on the same
    // device, so anything else means the shader read something that
    // varies per invocation.
    std::vector<float> texelPdf(kEnvTexels, 0.0f);
    std::vector<uint8_t> texelSeen(kEnvTexels, 0u);
    double maxCentreError = 0.0;
    float minPdf = std::numeric_limits<float>::infinity();
    float maxPdf = 0.0f;

    uint32_t srcQueueBase = kCapacity;
    uint32_t srcCountSlot = ohao::diff::WavefrontBuffers::kNextCountSlot;
    uint32_t dstQueueBase = 0u;
    uint32_t dstCountSlot = ohao::diff::WavefrontBuffers::kCurrentCountSlot;

    for (uint32_t d = 0; d < kEnvDispatches; ++d) {
        std::vector<uint32_t> outQueue;
        std::vector<float> outDraws;
        std::vector<float> envSamples;
        if (!ctx.runWavefrontScatterProbe(wf, srcQueueBase, srcCountSlot, dstQueueBase,
                                          dstCountSlot, /*albedo=*/1.0f, kIterationSeed,
                                          outQueue, outDraws, /*material=*/{}, &envSamples)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling scatter dispatch %u\n",
                         d);
            wf.destroy(ctx.allocator());
            return false;
        }
        const std::uint32_t requeued = wf.readbackCounter(ctx.allocator(), dstCountSlot);
        if (requeued != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: env sampling dispatch %u re-queued %u paths, "
                         "expected all %u -- the sample count the chi-squared bound is "
                         "derived for would not be the count actually drawn\n",
                         d, requeued, kCapacity);
            wf.destroy(ctx.allocator());
            return false;
        }
        if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: env sampling dispatch %u returned %zu env "
                         "sample floats, expected %u\n",
                         d, envSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats);
            wf.destroy(ctx.allocator());
            return false;
        }

        for (uint32_t i = 0; i < kCapacity; ++i) {
            const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
            const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
            const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
            const float pdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];

            const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                             "direction (%.9g,%.9g,%.9g) has length %.9g, expected a unit "
                             "vector\n",
                             d, i, dx, dy, dz, len);
                wf.destroy(ctx.allocator());
                return false;
            }
            if (!(pdf > 0.0f) || !std::isfinite(pdf)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                             "returned pdf %.9g. Every texel of this environment has strictly "
                             "positive luminance, so no sampled direction may have zero or "
                             "non-finite density -- a zero here is a division waiting to "
                             "happen in Task 4's estimator\n",
                             d, i, static_cast<double>(pdf));
                wf.destroy(ctx.allocator());
                return false;
            }
            minPdf = std::min(minPdf, pdf);
            maxPdf = std::max(maxPdf, pdf);

            // Invert equirectPixelToDir. This is the same inverse
            // pdfEnvMap performs, written from the forward map's
            // definition rather than copied out of it -- and written ONCE,
            // in oracleEnvTexelOf, rather than here and again in checks
            // 27, 31 and 33/34. All four rest on the binning agreeing.
            const OracleEnvTexel texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH);
            const double fx = texel.fx;
            const double fy = texel.fy;
            const int ix = texel.ix;
            const int iy = texel.iy;
            // A direction that is NOT a texel centre means the forward
            // map and this inverse disagree, and every bin index below
            // would then be meaningless -- so this is checked before the
            // count is taken, not after.
            const double centreError = texel.centreError;
            if (!(centreError <= kCentreSlack)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                             "direction (%.9g,%.9g,%.9g) inverts to (%.6f, %.6f) in texel "
                             "units, which is %.3g away from the centre of texel (%d, %d). "
                             "equirectPixelToDir emits texel CENTRES, so this is a "
                             "disagreement between the forward map and its inverse, not "
                             "rounding\n",
                             d, i, dx, dy, dz, fx, fy, centreError, ix, iy);
                wf.destroy(ctx.allocator());
                return false;
            }
            maxCentreError = std::max(maxCentreError, centreError);

            const std::size_t k = texel.index;
            binCount[k] += 1u;
            if (texelSeen[k] == 0u) {
                texelSeen[k] = 1u;
                texelPdf[k] = pdf;
            } else if (texelPdf[k] != pdf) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: texel (%d, %d) was returned with two "
                             "different pdfs, %.9g and %.9g. The pdf is a pure function of "
                             "the two CDF arrays and the texel index, so it cannot vary "
                             "between invocations unless the shader read something that "
                             "does\n",
                             ix, iy, static_cast<double>(texelPdf[k]),
                             static_cast<double>(pdf));
                wf.destroy(ctx.allocator());
                return false;
            }
        }

        std::swap(srcQueueBase, dstQueueBase);
        std::swap(srcCountSlot, dstCountSlot);
    }

    wf.destroy(ctx.allocator());

    // Checks 24, 25 and 26 are three INDEPENDENT verdicts computed from
    // this same run's 24576 samples: 24 from binCount, 25 from
    // minPdf/maxPdf (collected per-sample above, before any binning),
    // 26 from texelPdf/texelSeen. Nothing below depends on an earlier
    // one of the three having passed, so a perturbation that fails one
    // must not stop the other two from being computed and reported --
    // otherwise a bug that also happens to trip the chi-squared can
    // never be shown to be (or not be) independently caught by the
    // pdf-ratio and integrate-to-1 identities too, which is exactly the
    // comparison Step 5's perturbation report depends on. Each verdict
    // is therefore evaluated and printed unconditionally; failures
    // accumulate in checksFailed, and the group returns non-zero once,
    // at the very end, only after all three have had their say. Every
    // assertion below is exactly as strong as it was before this
    // restructuring -- only the control flow between them changed.
    bool checksFailed = false;

    // --- 24. Pearson's chi-squared against the independent oracle. ---
    double chiSq = 0.0;
    uint32_t totalBinned = 0;
    uint32_t worstBin = 0;
    double worstTerm = -1.0;
    for (uint32_t k = 0; k < kEnvTexels; ++k) {
        const double expected = expectedP[k] * static_cast<double>(kSampleCount);
        const double diff = static_cast<double>(binCount[k]) - expected;
        const double term = diff * diff / expected;
        chiSq += term;
        totalBinned += binCount[k];
        if (term > worstTerm) {
            worstTerm = term;
            worstBin = k;
        }
    }
    const double df = static_cast<double>(kEnvTexels) - 1.0;
    const double whT = 2.0 / (9.0 * df);
    const double chiSqCritical =
        df * std::pow(1.0 - whT + kChiSqZ * std::sqrt(whT), 3.0);
    if (totalBinned != kSampleCount) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: binned %u env samples, expected %u -- the "
                     "chi-squared statistic is only multinomial if every sample landed in "
                     "exactly one bin\n",
                     totalBinned, kSampleCount);
        checksFailed = true;
    } else if (!(chiSq <= chiSqCritical)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: env importance sampling chi-squared = %.4f over "
                     "%u samples in %u bins (df %.0f) exceeds the %.4f rejection threshold "
                     "(alpha = %.0e, Wilson-Hilferty). The empirical distribution of "
                     "sampleEnvMap's directions does not match the sin(theta)-weighted "
                     "luminance of the environment. Worst bin %u (texel %u, %u): observed "
                     "%u, expected %.3f, contributing %.3f\n",
                     chiSq, kSampleCount, kEnvTexels, df, chiSqCritical, kChiSqAlpha, worstBin,
                     worstBin % kEnvW, worstBin / kEnvW, binCount[worstBin],
                     expectedP[worstBin] * static_cast<double>(kSampleCount), worstTerm);
        checksFailed = true;
    } else {
        std::printf("[diff_gpu_probe] OK: env importance sampling matches an independent "
                    "sin(theta)-weighted-luminance oracle -- chi-squared %.4f over %u samples "
                    "in %u bins (df %.0f), below the derived %.4f threshold (alpha %.0e via "
                    "Wilson-Hilferty; least likely bin expected %.2f >= %.1f, so Pearson's "
                    "approximation holds); worst bin %u observed %u vs expected %.2f\n",
                    chiSq, kSampleCount, kEnvTexels, df, chiSqCritical, kChiSqAlpha,
                    minExpectedCount, kMinExpectedPerBin, worstBin, binCount[worstBin],
                    expectedP[worstBin] * static_cast<double>(kSampleCount));
    }

    // --- 25. Every returned pdf strictly positive (asserted per sample
    // above) and every direction a texel centre (likewise), plus the
    // pdf's SHAPE: it must be proportional to luminance alone.
    //
    // This is the sin(theta) Jacobian stated as an equality rather than
    // as an integral. The CDF's texel probability is proportional to
    // L * sin(theta_y); sampleEnvMap divides by
    // 2 pi^2 sin(theta_y) to get a solid-angle density, so the sine
    // cancels EXACTLY and
    //
    //     pdf(x,y) proportional to L(x,y),   independent of the row.
    //
    // The ratio of the largest returned pdf to the smallest must
    // therefore equal the luminance range of the map, a number this test
    // knows from the image it built and never from the shader. A sine
    // applied once too often or once too few -- in the builder or in the
    // shader -- breaks this while leaving both positivity and the
    // integral-to-1 identity of check 26 intact.
    //
    // TOLERANCE. The dominant error is float32 cancellation in the CDF
    // differences: the least likely texel has condDiff ~ 0.036 and
    // margDiff ~ 0.018 formed by subtracting values near 1, each with
    // absolute error up to 2^-23, giving relative errors of ~3.4e-6 and
    // ~6.6e-6. The ratio compounds two such pdfs, so ~2e-5. Asserted at
    // 1e-4, five times that; the observed value is printed.
    constexpr double kPdfRatioTolerance = 1e-4;
    double lumMin = 1e300;
    double lumMax = 0.0;
    for (uint32_t k = 0; k < kEnvTexels; ++k) {
        lumMin = std::min(lumMin, envLum[k]);
        lumMax = std::max(lumMax, envLum[k]);
    }
    const double lumRatio = lumMax / lumMin;
    const double pdfRatio = static_cast<double>(maxPdf) / static_cast<double>(minPdf);
    if (!(std::abs(pdfRatio / lumRatio - 1.0) <= kPdfRatioTolerance)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: the returned env pdfs span a ratio of %.9f, but "
                     "the environment's luminance spans %.9f. The solid-angle pdf must be "
                     "proportional to luminance ALONE -- the sin(theta) in the CDF's texel "
                     "probability is cancelled exactly by the sin(theta) in the "
                     "UV-to-solid-angle Jacobian -- so a different ratio means the sine was "
                     "applied a different number of times on the two sides (relative "
                     "difference %.3g, tolerance %.3g)\n",
                     pdfRatio, lumRatio, std::abs(pdfRatio / lumRatio - 1.0),
                     kPdfRatioTolerance);
        checksFailed = true;
    } else {
        std::printf("[diff_gpu_probe] OK: all %u returned env pdfs are finite and strictly "
                    "positive (min %.9g, max %.9g), their %.6f:1 range matches the map's own "
                    "%.6f:1 luminance range to %.3g (tolerance %.3g -- the sin(theta) "
                    "Jacobian cancels exactly), and every sampled direction inverts to a "
                    "texel centre within %.3g (slack %.3g)\n",
                    kSampleCount, static_cast<double>(minPdf), static_cast<double>(maxPdf),
                    pdfRatio, lumRatio, std::abs(pdfRatio / lumRatio - 1.0),
                    kPdfRatioTolerance, maxCentreError, kCentreSlack);
    }

    // --- 26. The returned pdfs integrate to 1 over the sphere. ---
    uint32_t unseen = 0;
    for (uint32_t k = 0; k < kEnvTexels; ++k) {
        if (texelSeen[k] == 0u) ++unseen;
    }
    if (unseen != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %u of %u texels were never sampled, so the pdf "
                     "sum below would be missing their contribution and could not be "
                     "compared against 1. Every texel's expected count is at least %.2f, so "
                     "this is not chance\n",
                     unseen, kEnvTexels, minExpectedCount);
        checksFailed = true;
    } else {
        const double dOmegaScale = (2.0 * kPi / static_cast<double>(kEnvW)) *
                                   (kPi / static_cast<double>(kEnvH));
        double pdfIntegral = 0.0;
        for (uint32_t y = 0; y < kEnvH; ++y) {
            for (uint32_t x = 0; x < kEnvW; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
                pdfIntegral += static_cast<double>(texelPdf[k]) * dOmegaScale * sinThetaRow[y];
            }
        }
        if (!(std::abs(pdfIntegral - 1.0) <= kPdfSumTolerance)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: the pdfs sampleEnvMap returned integrate to "
                         "%.9f over the sphere, not 1 within %.3g. pdf * dOmega is condDiff * "
                         "margDiff exactly (the sin(theta) cancels), so the total is the "
                         "CDF's own mass and any departure beyond float32 rounding is a "
                         "normalisation error, not quadrature\n",
                         pdfIntegral, kPdfSumTolerance);
            checksFailed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: the env pdfs integrate to %.9f over the sphere "
                        "(|deviation| %.3g, derived float32 budget ~4e-6, asserted %.3g) "
                        "across all %u texels\n",
                        pdfIntegral, std::abs(pdfIntegral - 1.0), kPdfSumTolerance,
                        kEnvTexels);
        }
    }

    // All three verdicts are computed and printed above regardless of
    // one another's outcome; only now, after all three have reported,
    // does the group return non-zero if any failed.
    if (checksFailed) {
        return false;
    }
    return true;
}

bool checkEnvPushFillAndShadowRay(ohao::diff::GpuProbeContext& ctx) {
    // ------------------------------------------------------------------
    // 27. WavefrontLoop::record's OWN push-constant fill (Stage 0b-2b Task 3
    //     fix, finding 2).
    // ------------------------------------------------------------------
    //
    // ohao/diff/wavefront/wavefront_loop.cpp fills ScatterPush's
    // envWidth/envHeight/envIntegral tail from `buffers` at record()'s own
    // call site. Checks 24-26 above never exercise that line: they run
    // through GpuProbeContext::runWavefrontScatterProbe, which fills
    // ScatterPush ITSELF (gpu_probe_context.cpp), at a different call site
    // entirely. The only caller of record() anywhere in this file is
    // runWavefrontFusedLoopProbe, which -- until this check was added --
    // built its WavefrontBuffers at the default 1x1 environment and never
    // read binding 6 back, so a transposed envWidth/envHeight inside
    // record() was UNOBSERVABLE: at 1x1 the two dimensions are
    // interchangeable and every direction still lands on the same one
    // texel.
    //
    // This check gives the fused loop a genuinely NON-SQUARE environment
    // (kEnvW != kEnvH) and reads the env-sample sink back after running
    // record() once, so a W<->H swap is no longer symmetric.
    //
    // THE ORACLE. wf.build(allocator, capacity, kEnvW, kEnvH) with no
    // uploadEnvironment() call afterward seeds the UV-uniform CDF
    // wavefront_buffers.cpp documents: cond[y][x] = (x+1)/W, marg[y] =
    // (y+1)/H (the SAME sampler task-3-report.md's Step 2 used to show the
    // chi-squared test has discriminating power the ratio/integral checks
    // alone do not -- see task-3-fix-report.md for that correction). Its CDF
    // texel probability is uniform in UV, p_CDF(x,y) = margDiff * condDiff =
    // (1/H)(1/W), so sampleEnvMap's pdf collapses to a CLOSED FORM that
    // depends on nothing but which row y the texel is in:
    //
    //     pdfUV = condDiff * margDiff * W * H = 1
    //     pdf = pdfUV / (2 pi^2 sin(theta_y)) = 1 / (2 pi^2 sin(theta_y))
    //
    // -- no CDF builder, no luminance image, just kEnvW, kEnvH and
    // elementary trigonometry, computed here independently of anything the
    // GPU did. If record() ever swaps envWidth and envHeight in the push
    // constants it fills, sampleEnvMap's two binary searches run against the
    // WRONG bound for each CDF array (the marginal array actually holds
    // envHeight() entries; searching it as if it held envWidth() either
    // walks off the array or stops short), so the returned direction stops
    // being the centre of any texel under the buffers' REAL (kEnvW, kEnvH)
    // -- caught by the texel-centre check below, the same technique check 25
    // uses -- and the closed-form pdf above stops matching.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
    constexpr uint32_t kCapacity = kW * kH;  // 512
    constexpr float kAlbedo = 0.5f;
    constexpr uint32_t kIterationSeed = 20260828u;
    constexpr uint32_t kBounces = 1;  // One dispatch through record() is enough.

    // Non-square on purpose -- see the comment above. A W<->H swap
    // inside record() is invisible whenever kEnvW == kEnvH.
    constexpr uint32_t kEnvW = 16;
    constexpr uint32_t kEnvH = 4;
    static_assert(kEnvW != kEnvH,
                 "check 27 needs a non-square environment to detect a W<->H swap");

    constexpr double kPi = 3.14159265358979323846;
    // Same basis as check 25's texel-centre slack: three orders of
    // magnitude of headroom over the float32 round trip, five hundred
    // times tighter than the half-texel that would make the bin
    // ambiguous.
    constexpr double kCentreSlack = 1e-3;
    // Same float32-cancellation budget as checks 25/26 (differences of
    // CDF entries, a W*H product, a division by a float32 sin(theta));
    // asserted at the same order of magnitude check 25 uses for its own
    // pdf comparison.
    constexpr double kPdfRelTolerance = 1e-4;

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 27 buffers build\n");
        return false;
    }

    std::vector<std::vector<float>> drawsPerBounce;
    std::vector<uint32_t> liveCountPerRun;
    std::vector<uint32_t> finalQueue;
    std::vector<float> envSamples;
    // The same run also produces check 28's evidence -- see below; one
    // dispatch, two independent verdicts.
    std::vector<float> neeSamples;
    if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                        drawsPerBounce, liveCountPerRun, finalQueue,
                                        &envSamples, &neeSamples)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 27 fused loop dispatch\n");
        wf.destroy(ctx.allocator());
        return false;
    }
    wf.destroy(ctx.allocator());

    if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 27 env samples readback returned %zu "
                     "floats, expected %u\n",
                     envSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats);
        return false;
    }

    double maxCentreError = 0.0;
    double maxPdfRelError = 0.0;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
        const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
        const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
        const float pdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];

        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 path %u env sample direction "
                         "(%.9g,%.9g,%.9g) has length %.9g, expected a unit vector\n",
                         i, dx, dy, dz, len);
            return false;
        }
        if (!(pdf > 0.0f) || !std::isfinite(pdf)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 path %u env sample returned pdf "
                         "%.9g -- the UV-uniform CDF wf.build seeds by default has strictly "
                         "positive probability everywhere\n",
                         i, static_cast<double>(pdf));
            return false;
        }

        // Invert equirectPixelToDir exactly as check 25 does -- through
        // the same host-side function, so "exactly as" is enforced rather
        // than asserted in a comment.
        const OracleEnvTexel texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH);
        const double fx = texel.fx;
        const double fy = texel.fy;
        const int ix = texel.ix;
        const int iy = texel.iy;
        const double centreError = texel.centreError;
        if (!(centreError <= kCentreSlack)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 (WavefrontLoop::record's "
                         "envWidth/envHeight fill): path %u's env sample direction "
                         "(%.9g,%.9g,%.9g) inverts to (%.6f, %.6f) in %ux%u texel units, "
                         "%.3g away from the nearest texel centre (%d, %d). record() fills "
                         "ScatterPush's envWidth/envHeight from `buffers`, and this "
                         "environment is non-square (%u != %u) specifically so a transposed "
                         "fill cannot land on a valid texel by symmetry\n",
                         i, dx, dy, dz, fx, fy, kEnvW, kEnvH, centreError, ix, iy, kEnvW,
                         kEnvH);
            return false;
        }
        maxCentreError = std::max(maxCentreError, centreError);

        const double thetaY =
            kPi * (static_cast<double>(iy) + 0.5) / static_cast<double>(kEnvH);
        const double expectedPdf = 1.0 / (2.0 * kPi * kPi * std::sin(thetaY));
        const double relErr =
            std::abs(static_cast<double>(pdf) - expectedPdf) / expectedPdf;
        if (!(relErr <= kPdfRelTolerance)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 (WavefrontLoop::record's "
                         "envWidth/envHeight fill): path %u's env pdf is %.9g, expected "
                         "%.9g (1/(2 pi^2 sin(theta)) for the UV-uniform CDF wf.build seeds "
                         "by default) -- relative error %.3g, tolerance %.3g\n",
                         i, static_cast<double>(pdf), expectedPdf, relErr, kPdfRelTolerance);
            return false;
        }
        maxPdfRelError = std::max(maxPdfRelError, relErr);
    }
    std::printf("[diff_gpu_probe] OK: check 27 -- WavefrontLoop::record's OWN "
                "envWidth/envHeight fill of ScatterPush (the production call site, not "
                "runWavefrontScatterProbe's hand-filled one behind checks 24-26) reaches "
                "wf_scatter.comp intact: all %u env samples from a %ux%u NON-SQUARE "
                "environment invert to a texel centre (max error %.3g, slack %.3g) and match "
                "the closed-form UV-uniform pdf 1/(2 pi^2 sin(theta)) to %.3g relative "
                "(tolerance %.3g)\n",
                kCapacity, kEnvW, kEnvH, maxCentreError, kCentreSlack, maxPdfRelError,
                kPdfRelTolerance);

    // ------------------------------------------------------------------
    // 28. The shadow ray is actually traced (Stage 0b-2b Task 4).
    // ------------------------------------------------------------------
    //
    // Check 27's run is the CLOSED BOX, entered from its centre. A ray
    // leaving any point strictly inside a closed convex body through any
    // direction hits a face -- that is the same geometric fact the
    // fused-loop survival theorem rests on -- so EVERY shadow ray
    // wf_scatter.comp's next-event estimator traces here is occluded,
    // and every direct-lighting contribution in the binding-7 record
    // must be EXACTLY zero. Not "small": zero, bit for bit, because
    // diffMisTerm multiplies by the visibility term rather than
    // attenuating by it.
    //
    // This is the check that the shadow ray EXISTS. A visibility term
    // stuck at 1 -- the shape a missing or mis-flagged ray query takes
    // -- produces a perfectly plausible unoccluded estimate that checks
    // 29-31 (which run in an unoccluded scene, where the right answer IS
    // visibility 1) could never distinguish from the truth. The two
    // scenes are complementary on purpose: one where the answer must be
    // 1 everywhere and one where it must be 0 everywhere.
    //
    // NON-VACUITY. "All contributions are zero" is also what a zeroed
    // buffer looks like. So the recovered environment radiance is
    // asserted STRICTLY POSITIVE on the same samples: with radiance > 0,
    // a nonzero BSDF and directions above the horizon, the visibility
    // term is the only factor that can be zeroing the product. (The
    // default UV-uniform CDF this check runs against has integral 1 and
    // strictly positive density in every texel -- see
    // wavefront_buffers.cpp's seeding -- so a zero radiance here would
    // itself be a failure.)
    if (neeSamples.size() !=
        static_cast<std::size_t>(kCapacity) * ohao::diff::kNeeSampleFloats) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 28 NEE samples readback returned %zu "
                     "floats, expected %u\n",
                     neeSamples.size(),
                     kCapacity * ohao::diff::kNeeSampleFloats);
        return false;
    }
    {
        uint32_t litSamples = 0;
        uint32_t surfaceSamples = 0;
        double minEnvRadiance = std::numeric_limits<double>::infinity();
        for (uint32_t i = 0; i < kCapacity; ++i) {
            const std::size_t b =
                static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
            if (neeSamples[b + ohao::diff::kNeeSlotSurfaceBranch] == 0.0f) continue;
            ++surfaceSamples;
            const float visLight = neeSamples[b + ohao::diff::kNeeSlotVisLight];
            const float visBsdf = neeSamples[b + ohao::diff::kNeeSlotVisBsdf];
            if (visLight != 0.0f || visBsdf != 0.0f) ++litSamples;
            minEnvRadiance = std::min(
                minEnvRadiance,
                static_cast<double>(neeSamples[b + ohao::diff::kNeeSlotEnvRadiance]));
            for (uint32_t c = 0; c < 3u; ++c) {
                const float nee =
                    neeSamples[b + ohao::diff::kNeeSlotNeeUnweighted + c];
                const float bsdf =
                    neeSamples[b + ohao::diff::kNeeSlotBsdfUnweighted + c];
                if (nee != 0.0f || bsdf != 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 28 -- path %u reported a "
                                 "nonzero direct-lighting contribution (nee %.9g, bsdf %.9g, "
                                 "channel %u) from INSIDE a closed box, where every shadow "
                                 "ray must be occluded. Its visibility terms are %.9g and "
                                 "%.9g\n",
                                 i, static_cast<double>(nee), static_cast<double>(bsdf), c,
                                 static_cast<double>(visLight),
                                 static_cast<double>(visBsdf));
                    return false;
                }
            }
        }
        if (surfaceSamples != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 28 -- only %u of %u paths took "
                         "wf_scatter.comp's surface branch. Every ray from the centre of a "
                         "closed box hits a face, so a miss here means the scene or the "
                         "trace is not what this check assumes and the zero contributions "
                         "below would be vacuous\n",
                         surfaceSamples, kCapacity);
            return false;
        }
        if (litSamples != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 28 -- %u of %u paths reported an "
                         "UNOCCLUDED shadow ray from inside a closed box\n",
                         litSamples, kCapacity);
            return false;
        }
        if (!(minEnvRadiance > 0.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 28 -- the least of the recovered "
                         "environment radiances is %.9g. The zero contributions above would "
                         "then be zero because there is no light, not because the shadow ray "
                         "found geometry, and this check would prove nothing\n",
                         minEnvRadiance);
            return false;
        }
        std::printf("[diff_gpu_probe] OK: check 28 -- every one of %u paths inside a CLOSED "
                    "box reports visibility exactly 0 for both the light sample and the BSDF "
                    "sample, and every direct-lighting contribution is exactly 0.0 (not "
                    "merely small), while the recovered environment radiance is strictly "
                    "positive (min %.6g) -- so the zeros are the shadow ray's doing and not "
                    "an absence of light\n",
                    kCapacity, minEnvRadiance);
    }
    return true;
}

}  // namespace ohao::diff::probe
