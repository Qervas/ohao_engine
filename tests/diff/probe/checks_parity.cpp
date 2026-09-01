// The stage gate, checks 33-34: the whole wavefront integrator against an
// INDEPENDENT CPU reference path tracer, per pixel and pooled.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_parity.hpp"

#include "probe/oracle_bsdf.hpp"
#include "probe/oracle_integrator.hpp"
#include "probe/scene.hpp"

#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkIntegratorParity(ohao::diff::GpuProbeContext& ctx) {
    // -----------------------------------------------------------------
    // 33-34. THE STAGE GATE: the wavefront integrator against an
    // independent reference integrator, on a probe-owned scene.
    // -----------------------------------------------------------------
    //
    // WHAT THE REFERENCE IS, AND WHY IT IS NOT ohao::PathTracer. Stated in
    // full at the head of this file's anonymous namespace (see "INDEPENDENT
    // CPU REFERENCE INTEGRATOR"): PathTracer is a
    // VK_KHR_ray_tracing_pipeline renderer and GpuProbeContext's device
    // deliberately does not enable that extension, which this task's
    // constraints forbid adding. The reference is therefore a CPU path
    // tracer written from the definition of the estimator, sharing no code,
    // no constant and no sampling machinery with the GPU integrator -- its
    // own intersector, its own basis, its own RNG, cosine sampling with NO
    // environment importance sampling and NO MIS. Two different estimators
    // of one integral agree only if both are unbiased.
    //
    // WHAT IS COMPARED, AND WHAT IS EXCLUDED. The film holds only
    // SUM_k T_k * Lhat_direct(x_k): MIS direct lighting at surface vertices,
    // truncated at kBounces, with no emissive term and no background term.
    // The reference computes exactly that, with the same truncation and the
    // same two omissions. The one term a classical path tracer would have
    // and this film does not is the CAMERA ray's own miss -- and the scene
    // below is built so that set is EMPTY, which the first assertion here
    // measures rather than assumes (a one-bounce run must leave all
    // kCapacity paths live). A ray that escapes at a LATER bounce is not a
    // gap at all: its environment contribution was already added, eagerly,
    // at the vertex it left, by the BSDF strategy's shadow ray along that
    // very direction. The anonymous-namespace header derives that identity.
    //
    // THE SCENE, and why each piece is where it is.
    //   * FLOOR, y = 0, |x|,|z| <= 8. Every primary ray lands on it: the
    //     camera sits at y = 3 looking straight down with tanHalfFov 0.2 at
    //     aspect 8, so the extreme ray lands at |x| = 3*0.984375*8*0.2 =
    //     4.725 and |z| = 0.525 -- 3.27 units inside the nearest edge. No
    //     primary ray is anywhere near a silhouette, which matters because
    //     a pixel whose primary ray grazed an edge could hit different
    //     triangles under a BVH and under Moller-Trumbore and would show up
    //     as a permanent per-pixel disagreement that no sample count fixes.
    //   * OVERHANG, y = 5, |x| <= 1.5. ABOVE the camera, and every primary
    //     ray travels strictly downward, so it is unreachable by a primary
    //     ray by construction -- but it occludes the zenith (the most
    //     cosine-weighted part of the hemisphere) for the middle of the
    //     image and catches second-bounce rays. This is where most of the
    //     visibility signal and most of the interreflection come from.
    //   * SIDE WALL, x = 5.5, 0 <= y <= 4. Out of the primary frustum with
    //     margin (a ray needs a horizontal slope of 5.5/3 = 1.833 to reach
    //     it and the widest is 1.575), and it makes the occlusion vary
    //     ASYMMETRICALLY across the image: a floor point at x = +4.725 is
    //     0.775 from it, one at x = -4.725 is 10.2 away.
    // Every quad is wound so that wf_intersect.comp's flip-to-oppose-the-ray
    // step fires for the rays that actually reach it.
    //
    // THE ENVIRONMENT is a smooth, strictly positive, doubly asymmetric
    // gradient (brightest at the +Y pole, where the floor can see it) with a
    // 5:1 contrast. That contrast is a DESIGN CALL: a strongly peaked
    // environment -- checks 29-31 use one with an 8x block -- inflates the
    // variance of BOTH estimators, and the variance is what sets how small a
    // disagreement this gate can resolve. Env importance sampling, pdfEnvMap
    // and the balance heuristic are all still exercised (the CDF is not
    // uniform in either axis); it is only their variance-reduction margin
    // that is smaller here, and no claim is made about that.
    //
    // THE BOUND, DERIVED. Two independent unbiased estimators of the same
    // per-pixel value mu(p):
    //   * GPU: kSeedsFull runs, each ONE sample per pixel, each with a
    //     distinct iterationSeed. wf_scatter.comp rebuilds its RNG from
    //     (pixelIndex, sampleIndex, iterationSeed) every bounce, so distinct
    //     seeds give independent paths. Sample mean mG(p), unbiased sample
    //     variance vG(p), standard error seG = sqrt(vG/R).
    //   * CPU: kCpuFull samples, its own generator. mC(p), seC = sqrt(vC/M).
    // D(p) = mG(p) - mC(p) has expectation 0 and standard deviation
    // sigma(p) = sqrt(seG^2 + seC^2), ESTIMATED FROM THE RUN ITSELF -- this
    // is the "variance estimate from the run" option Task 6 names, taken
    // deliberately: the per-pixel variance of an MIS path-tracing estimator
    // on this geometry has no closed form to derive it from. What IS derived
    // is the multiplier:
    //   * PER PIXEL, kPixels comparisons, two-sided. A family-wise false
    //     rejection rate of 1e-3 needs 2*kPixels*Phi(-z) <= 1e-3, i.e.
    //     Phi(-z) <= 9.8e-7, i.e. z >= 4.76. kPerPixelZ = 5.0 (family-wise
    //     2*512*Phi(-5) = 2*512*2.8665e-7 ~= 2.94e-4 under normality; still
    //     comfortably under the 1e-3 budget). Normality is approximate at these
    //     sample counts, which is why the pooled test below -- where the
    //     central limit theorem is on much firmer ground -- carries the
    //     sharp discrimination and this one carries the spatial coverage.
    //   * POOLED, ONE comparison, on the IMAGE TOTAL. For each GPU seed i
    //     the whole-image total S_i = sum_p film_i(p) is one draw; for each
    //     CPU sample index j, T_j = sum_p c(p,j) is one draw. Their sample
    //     variances are computed ACROSS RUNS, so this statistic needs no
    //     assumption at all about whether different pixels are independent
    //     -- any correlation is already inside Var(S_i). One comparison at
    //     1e-4 two-sided needs z >= 3.9; kPooledZ = 4.0.
    // No tolerance here was chosen by running the check and widening until
    // it passed; both multipliers come from the comparison count, and both
    // scales come from measured variances.
    //
    // WHY BOTH. The per-pixel test sees a disagreement that lives in a few
    // pixels and cancels in the mean; the pooled test sees a coherent bias
    // far too small for any single pixel to resolve (a wrong material
    // constant, a double-counted throughput, an MIS partition that does not
    // sum to one). Its resolution is reported as a number below and gated:
    // if 4*sigma_pooled/mean ever exceeded kMaxPooledResolution the check
    // refuses to claim a verdict, because "it passed" would then be
    // compatible with "there was nothing it could have detected".
    //
    // CONVERGENCE. Everything above is also computed on a PREFIX of both
    // sample sets -- a quarter of the seeds and a quarter of the CPU samples
    // -- and the check asserts that the root-mean-square of D over the image
    // SHRINKS BY THE PREDICTED FACTOR. rms(D)^2 estimates E[D^2] =
    // sigma^2, and quadrupling both sample counts quarters sigma^2, so the
    // predicted ratio is exactly 0.5. A FIXED BIAS b would floor rms(D) at
    // |b| and drive that ratio to 1: this is the assertion a pair of
    // wrong-but-close images cannot satisfy, and the reason a fixed
    // tolerance alone would not be a gate. The window is [0.30, 0.70]: the
    // sampling standard deviation of an RMS over kPixels values is about
    // sqrt(1/(2*kPixels)) ~ 3% relative, and the two sets are NESTED (the
    // prefix is literally the first quarter of the same runs), so the ratio
    // is far more stable than that; the window is widened well past it to
    // cover the non-Gaussian tail of a Monte Carlo difference. The lower
    // edge is not decoration -- a ratio near zero would mean D collapsed,
    // which is what comparing a quantity against itself looks like.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kPixels = kW * kH;
    constexpr uint32_t kBounces = 3;
    constexpr float kAlbedo = 0.6f;

    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
    // (the texel count is no longer needed here: buildParityEnvironment
    // sizes both of its outputs from kEnvW/kEnvH itself)

    constexpr std::size_t kSeedsFull = 512;
    constexpr std::size_t kSeedsPrefix = kSeedsFull / 4;
    constexpr std::size_t kCpuFull = 4096;
    constexpr std::size_t kCpuPrefix = kCpuFull / 4;
    static_assert(kSeedsFull == 4 * kSeedsPrefix && kCpuFull == 4 * kCpuPrefix,
                  "the convergence assertion predicts a factor of exactly 2 in rms(D), "
                  "which requires BOTH sample counts to scale by exactly 4");

    constexpr double kPerPixelZ = 5.0;
    constexpr double kPooledZ = 4.0;
    constexpr double kConvergenceMin = 0.30;
    constexpr double kConvergenceMax = 0.70;
    // Pre-registered non-vacuity gates. These are statements about how
    // sharp this check has to be before its verdict means anything, not
    // tolerances on the thing under test.
    constexpr double kMaxPooledResolution = 0.02;   // 4*sigma/mean on the image total
    constexpr double kMaxPerPixelResolution = 0.30;  // worst 5*sigma(p)/mu(p)
    // A floor under sigma(p), for the arithmetic corner where a pixel's
    // estimated variance underflows to something meaningless. Expressed
    // as a fraction of the image's own mean pixel value so it carries no
    // absolute scale of its own. The check reports the smallest observed
    // sigma(p) as a multiple of it, so it is visible whether it ever
    // came close to binding.
    constexpr double kSigmaFloorFraction = 1e-6;

    // --- The environment, the scene and the camera. All three are built
    // by the shared builders in this file's anonymous namespace, NOT
    // transcribed here: the Stage 1 Task 2 gradient checks run against
    // the identical configuration, and two copies of a test scene are two
    // chances for one of them to drift into measuring something else. The
    // reasons each quad is where it is, and the reason the environment's
    // contrast is 5:1 rather than peaked, are on those builders.
    std::vector<float> envRgba;
    std::vector<double> envLum;
    buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);

    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildParityScene(positions, indices);
    const std::vector<ParityTriangle> refTris = parityTrianglesFromSoup(positions, indices);

    const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

    // Distinct, spread-out seeds. Any injective map would do; this one
    // is an odd stride so no two seeds collide and consecutive runs are
    // not neighbours in the RNG's input space.
    std::vector<uint32_t> seeds(kSeedsFull);
    for (std::size_t i = 0; i < kSeedsFull; ++i) {
        seeds[i] = 20260901u + static_cast<uint32_t>(i) * 2654435761u;
    }

    struct ParityConfig {
        const char* name;
        uint32_t checkNumber;
        ohao::diff::WavefrontScatterMaterial material;
    };
    // Two materials, so the gate covers both of diffBsdfSample's lobes.
    // The Lambert case is the one whose per-bounce estimator weight is
    // exactly `albedo` (checks 14/17); the conductor case has q = 1
    // exactly, so every sample comes from the GGX visible-normal
    // sampler and the diffuse lobe is entirely out of the picture.
    const ParityConfig configs[2] = {
        {"pure Lambert (roughness 1, metallic 0, specularWeight 0)", 33u, {1.0f, 0.0f, 0.0f}},
        {"rough conductor (roughness 0.7, metallic 1, specularWeight 1)", 34u,
         {0.7f, 1.0f, 1.0f}},
    };

    for (const ParityConfig& cfg : configs) {
        ohao::EnvCDF envCdf;
        envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!envCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u: EnvCDF::build produced no "
                                  "CDF for a %ux%u strictly-positive environment\n",
                         cfg.checkNumber, kEnvW, kEnvH);
            return false;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                  envCdf.integral())) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check %u buffers build / env CDF upload\n",
                         cfg.checkNumber);
            wf.destroy(ctx.allocator());
            return false;
        }

        // --- NON-VACUITY GATE 1: no primary ray escapes. One bounce,
        // one seed: wf_intersect.comp compacts only survivors and
        // wf_scatter.comp re-queues everything it is given, so the live
        // count after a ONE-bounce run is exactly the number of primary
        // rays that hit something. Anything below kCapacity means the
        // film is missing a directly-visible-environment term this
        // comparison does not model, and the verdict below would be
        // about a different quantity than the one it claims.
        std::vector<std::vector<float>> probeFilm;
        std::vector<uint32_t> probeLive;
        const std::span<const uint32_t> oneSeed(seeds.data(), 1);
        if (!ctx.runWavefrontParityProbe(wf, kW, kH, /*bounces=*/1u, camera,
                                         std::span<const float>(positions),
                                         std::span<const uint32_t>(indices), kAlbedo,
                                         cfg.material, oneSeed, probeFilm, probeLive)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u one-bounce probe dispatch\n",
                         cfg.checkNumber);
            wf.destroy(ctx.allocator());
            return false;
        }
        if (probeLive.size() != 1 || probeLive[0] != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check %u -- %u of %u primary rays hit the "
                         "scene. This check compares a film that contains NO "
                         "directly-visible-environment term against a reference that "
                         "contains none either, which is only the same quantity when every "
                         "primary ray hits. It does not, so the scene is wrong, not the "
                         "integrator\n",
                         cfg.checkNumber, probeLive.empty() ? 0u : probeLive[0], kCapacity);
            wf.destroy(ctx.allocator());
            return false;
        }

        // --- The GPU integrator: kSeedsFull independent 1-spp runs of
        // the full kBounces loop, through WavefrontLoop::record.
        std::vector<std::vector<float>> films;
        std::vector<uint32_t> liveCounts;
        if (!ctx.runWavefrontParityProbe(wf, kW, kH, kBounces, camera,
                                         std::span<const float>(positions),
                                         std::span<const uint32_t>(indices), kAlbedo,
                                         cfg.material,
                                         std::span<const uint32_t>(seeds), films, liveCounts)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u parity probe dispatch\n",
                         cfg.checkNumber);
            wf.destroy(ctx.allocator());
            return false;
        }
        wf.destroy(ctx.allocator());

        if (films.size() != kSeedsFull) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u got %zu films, expected %zu\n",
                         cfg.checkNumber, films.size(), kSeedsFull);
            return false;
        }

        // --- NON-VACUITY GATE 2: the three film channels. Both the base
        // colour and the environment are grey and every film factor is
        // applied per channel identically, so R, G and B must be the
        // SAME BITS. Asserting it costs nothing and is the only thing
        // here that would notice a per-channel indexing error; it also
        // licenses everything below comparing channel 0 alone.
        for (std::size_t i = 0; i < films.size(); ++i) {
            if (films[i].size() != static_cast<std::size_t>(kPixels) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check %u film %zu has %zu floats, "
                             "expected %u\n",
                             cfg.checkNumber, i, films[i].size(), kPixels * 3u);
                return false;
            }
            for (uint32_t p = 0; p < kPixels; ++p) {
                const float r = films[i][static_cast<std::size_t>(p) * 3u + 0u];
                const float g = films[i][static_cast<std::size_t>(p) * 3u + 1u];
                const float b = films[i][static_cast<std::size_t>(p) * 3u + 2u];
                if (!std::isfinite(r) || r < 0.0f || g != r || b != r) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check %u run %zu pixel %u film is "
                                 "(%.9g, %.9g, %.9g). It must be finite, non-negative and "
                                 "bit-identical across the three channels: this scene's base "
                                 "colour and environment are both grey\n",
                                 cfg.checkNumber, i, p, static_cast<double>(r),
                                 static_cast<double>(g), static_cast<double>(b));
                    return false;
                }
            }
        }

        // --- The reference integrator. Pixel-parallel; each pixel's RNG
        // stream depends only on its own index (see the `p`-only seed
        // below), so EVERY PER-PIXEL RESULT -- cpuSumPrefix/Full,
        // cpuSumSqPrefix/Full, and everything checks 33-34 derive from
        // them (worstZ, sharpness, per-pixel resolution) -- is exactly
        // thread-count-independent: which thread computes pixel p never
        // changes p's own running sum. The ONE quantity that is not is
        // cpuImageTotals[j] (the pooled statistic's CPU side, below):
        // it is a sum of per-thread partial sums, so its low-order bits
        // depend on how many threads split the reduction, i.e. on
        // hardware_concurrency(). At this scale (an image total of
        // order 361, threadCount <= 16) that is a ~1e-13 relative
        // floating-point reordering effect -- roughly 5e-14 absolute --
        // many orders below anything kPooledZ resolves, so it is
        // immaterial to the verdict, but it is not exactly zero.
        ParityRefScene refScene;
        refScene.tris = refTris;
        refScene.envLum = &envLum;
        refScene.envW = kEnvW;
        refScene.envH = kEnvH;
        refScene.material.baseColor = {kAlbedo, kAlbedo, kAlbedo};
        refScene.material.roughness = cfg.material.roughness;
        refScene.material.metallic = cfg.material.metallic;
        refScene.material.specularWeight = cfg.material.specularWeight;
        refScene.bounces = kBounces;

        const OracleVec3 camOrigin{camera.origin[0], camera.origin[1], camera.origin[2]};

        std::vector<double> cpuSumPrefix(kPixels, 0.0);
        std::vector<double> cpuSumSqPrefix(kPixels, 0.0);
        std::vector<double> cpuSumFull(kPixels, 0.0);
        std::vector<double> cpuSumSqFull(kPixels, 0.0);
        // Per CPU sample index, the whole-image total. Accumulated
        // thread-locally and reduced in thread order, so it is
        // deterministic.
        std::vector<double> cpuImageTotals(kCpuFull, 0.0);

        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned threadCount = std::min<unsigned>(hw, 16u);
        std::vector<std::vector<double>> perThreadTotals(
            threadCount, std::vector<double>(kCpuFull, 0.0));
        {
            std::vector<std::thread> workers;
            workers.reserve(threadCount);
            for (unsigned tIdx = 0; tIdx < threadCount; ++tIdx) {
                workers.emplace_back([&, tIdx]() {
                    const uint32_t begin =
                        static_cast<uint32_t>((static_cast<std::size_t>(kPixels) * tIdx) /
                                              threadCount);
                    const uint32_t end = static_cast<uint32_t>(
                        (static_cast<std::size_t>(kPixels) * (tIdx + 1u)) / threadCount);
                    std::vector<double>& totals = perThreadTotals[tIdx];
                    for (uint32_t p = begin; p < end; ++p) {
                        const uint32_t px = p % kW;
                        const uint32_t py = p / kW;
                        const OracleVec3 camDir = parityCameraRay(px, py, kW, kH, camera);
                        // Per-pixel stream, so the partition into
                        // threads cannot change any number.
                        std::mt19937_64 rng(0x9E3779B97F4A7C15ull *
                                                (static_cast<std::uint64_t>(p) + 1ull) +
                                            static_cast<std::uint64_t>(cfg.checkNumber) *
                                                1000003ull);
                        for (std::size_t j = 0; j < kCpuFull; ++j) {
                            const double v =
                                parityReferenceSample(refScene, camOrigin, camDir, rng);
                            cpuSumFull[p] += v;
                            cpuSumSqFull[p] += v * v;
                            if (j < kCpuPrefix) {
                                cpuSumPrefix[p] += v;
                                cpuSumSqPrefix[p] += v * v;
                            }
                            totals[j] += v;
                        }
                    }
                });
            }
            for (std::thread& w : workers) w.join();
        }
        for (unsigned tIdx = 0; tIdx < threadCount; ++tIdx) {
            for (std::size_t j = 0; j < kCpuFull; ++j) {
                cpuImageTotals[j] += perThreadTotals[tIdx][j];
            }
        }

        // --- Per-pixel statistics, at the prefix and at the full budget.
        std::vector<double> gpuPixel(kSeedsFull, 0.0);
        double meanPixelValue = 0.0;
        for (uint32_t p = 0; p < kPixels; ++p) {
            meanPixelValue += cpuSumFull[p] / static_cast<double>(kCpuFull);
        }
        meanPixelValue /= static_cast<double>(kPixels);
        const double sigmaFloor = kSigmaFloorFraction * std::abs(meanPixelValue);

        double worstZ[2] = {0.0, 0.0};
        uint32_t worstZPixel[2] = {0u, 0u};
        double sumDSq[2] = {0.0, 0.0};
        double worstPixelResolution = 0.0;
        double minSigma = std::numeric_limits<double>::infinity();
        double worstDetail[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        bool sigmaFloorBound = false;

        for (uint32_t p = 0; p < kPixels; ++p) {
            for (std::size_t i = 0; i < kSeedsFull; ++i) {
                gpuPixel[i] = films[i][static_cast<std::size_t>(p) * 3u + 0u];
            }
            for (int which = 0; which < 2; ++which) {
                const std::size_t r = (which == 0) ? kSeedsPrefix : kSeedsFull;
                const std::size_t m = (which == 0) ? kCpuPrefix : kCpuFull;
                double meanG = 0.0, varG = 0.0;
                parityMoments(gpuPixel.data(), r, meanG, varG);
                const double sumC = (which == 0) ? cpuSumPrefix[p] : cpuSumFull[p];
                const double sumSqC = (which == 0) ? cpuSumSqPrefix[p] : cpuSumSqFull[p];
                double meanC = 0.0, varC = 0.0;
                parityMomentsFromSums(sumC, sumSqC, m, meanC, varC);
                const double sigma = std::sqrt(varG / static_cast<double>(r) +
                                               varC / static_cast<double>(m));
                const double d = meanG - meanC;
                const double denom = std::max(sigma, sigmaFloor);
                if (sigma < sigmaFloor) sigmaFloorBound = true;
                const double z = std::abs(d) / denom;
                sumDSq[which] += d * d;
                if (z > worstZ[which]) {
                    worstZ[which] = z;
                    worstZPixel[which] = p;
                    if (which == 1) {
                        worstDetail[0] = meanG;
                        worstDetail[1] = meanC;
                        worstDetail[2] = d;
                        worstDetail[3] = sigma;
                        worstDetail[4] = std::sqrt(varG / static_cast<double>(r));
                        worstDetail[5] = std::sqrt(varC / static_cast<double>(m));
                    }
                }
                if (which == 1) {
                    minSigma = std::min(minSigma, sigma);
                    const double mu = std::max(std::abs(meanG), std::abs(meanC));
                    if (mu > 0.0) {
                        worstPixelResolution =
                            std::max(worstPixelResolution, kPerPixelZ * sigma / mu);
                    }
                }
            }
        }
        const double rmsPrefix = std::sqrt(sumDSq[0] / static_cast<double>(kPixels));
        const double rmsFull = std::sqrt(sumDSq[1] / static_cast<double>(kPixels));

        // --- The pooled statistic, on the IMAGE TOTAL. Its variance is
        // taken across whole runs, so it makes no independence
        // assumption about pixels.
        std::vector<double> gpuImageTotals(kSeedsFull, 0.0);
        for (std::size_t i = 0; i < kSeedsFull; ++i) {
            double s = 0.0;
            for (uint32_t p = 0; p < kPixels; ++p) {
                s += films[i][static_cast<std::size_t>(p) * 3u + 0u];
            }
            gpuImageTotals[i] = s;
        }
        double pooledZ[2] = {0.0, 0.0};
        double pooledDetail[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (int which = 0; which < 2; ++which) {
            const std::size_t r = (which == 0) ? kSeedsPrefix : kSeedsFull;
            const std::size_t m = (which == 0) ? kCpuPrefix : kCpuFull;
            double meanS = 0.0, varS = 0.0, meanT = 0.0, varT = 0.0;
            parityMoments(gpuImageTotals.data(), r, meanS, varS);
            parityMoments(cpuImageTotals.data(), m, meanT, varT);
            const double sigma =
                std::sqrt(varS / static_cast<double>(r) + varT / static_cast<double>(m));
            pooledZ[which] = (sigma > 0.0) ? std::abs(meanS - meanT) / sigma : 0.0;
            if (which == 1) {
                pooledDetail[0] = meanS;
                pooledDetail[1] = meanT;
                pooledDetail[2] = meanS - meanT;
                pooledDetail[3] = sigma;
                pooledDetail[4] = std::sqrt(varS / static_cast<double>(r));
                pooledDetail[5] = std::sqrt(varT / static_cast<double>(m));
            }
        }
        const double pooledResolution =
            (pooledDetail[0] != 0.0) ? kPooledZ * pooledDetail[3] / std::abs(pooledDetail[0])
                                     : std::numeric_limits<double>::infinity();

        const double convergenceRatio =
            (rmsPrefix > 0.0) ? rmsFull / rmsPrefix : std::numeric_limits<double>::infinity();

        const bool perPixelOk = worstZ[1] <= kPerPixelZ;
        const bool pooledOk = pooledZ[1] <= kPooledZ;
        const bool convergenceOk =
            convergenceRatio >= kConvergenceMin && convergenceRatio <= kConvergenceMax;
        const bool sharpEnough = pooledResolution <= kMaxPooledResolution &&
                                 worstPixelResolution <= kMaxPerPixelResolution;

        if (!perPixelOk || !pooledOk || !convergenceOk || !sharpEnough || sigmaFloorBound) {
            std::fprintf(
                stderr,
                "[diff_gpu_probe] FAIL: check %u -- wavefront integrator vs the independent "
                "CPU reference, %s, %u bounces, %zu GPU runs x 1 spp vs %zu reference "
                "samples per pixel:\n"
                "  PER PIXEL: worst |mean_gpu - mean_ref| / sigma = %.3f at pixel %u "
                "(allowed %.2f, family-wise 1e-3 over %u comparisons). There mean_gpu = "
                "%.9g, mean_ref = %.9g, difference %.4g, sigma %.4g (gpu %.4g, ref %.4g).\n"
                "  POOLED (image total, variance taken across whole runs so no "
                "pixel-independence assumption): |%.9g - %.9g| = %.4g, sigma %.4g (gpu %.4g, "
                "ref %.4g), z = %.3f (allowed %.2f).\n"
                "  CONVERGENCE: rms(D) went %.4g -> %.4g when both sample counts were "
                "quadrupled, ratio %.4f (predicted 0.5, window [%.2f, %.2f]). A ratio near 1 "
                "is a FIXED BIAS surviving more samples; a ratio near 0 is a difference that "
                "collapsed, which is what comparing a quantity against itself looks like.\n"
                "  SHARPNESS: the pooled test resolves a coherent bias of %.3g relative "
                "(gate %.3g); the worst pixel resolves %.3g relative (gate %.3g). A verdict "
                "is refused above those, because passing would then be compatible with there "
                "being nothing to detect.%s\n",
                cfg.checkNumber, cfg.name, kBounces, kSeedsFull, kCpuFull, worstZ[1],
                worstZPixel[1], kPerPixelZ, kPixels, worstDetail[0], worstDetail[1],
                worstDetail[2], worstDetail[3], worstDetail[4], worstDetail[5],
                pooledDetail[0], pooledDetail[1], pooledDetail[2], pooledDetail[3],
                pooledDetail[4], pooledDetail[5], pooledZ[1], kPooledZ, rmsPrefix, rmsFull,
                convergenceRatio, kConvergenceMin, kConvergenceMax, pooledResolution,
                kMaxPooledResolution, worstPixelResolution, kMaxPerPixelResolution,
                sigmaFloorBound ? "\n  The sigma floor BOUND on at least one pixel, so at "
                                  "least one comparison was made against an arithmetic "
                                  "guard rather than a measured variance."
                                : "");
            return false;
        }

        std::printf(
            "[diff_gpu_probe] OK: check %u -- the wavefront integrator (%zu independent "
            "1-spp runs through WavefrontLoop::record, %u bounces) and an INDEPENDENT CPU "
            "reference path tracer (%zu samples/pixel, its own intersector, basis, RNG and "
            "cosine-sampled MIS-free estimator -- no CDF, no pdfEnvMap, no balance "
            "heuristic) agree on a probe-owned scene, %s. All %u primary rays hit, so the "
            "film's missing directly-visible-environment term is identically zero here and "
            "the two sides hold the same quantity. Per pixel: worst |D|/sigma = %.3f at "
            "pixel %u over %u comparisons (bound %.2f, family-wise 1e-3); image total: "
            "%.6g vs %.6g, z = %.3f (bound %.2f). Agreement IMPROVES with samples: rms(D) "
            "%.4g -> %.4g on quadrupling both budgets, ratio %.4f against a predicted 0.5 "
            "(window [%.2f, %.2f]) -- a fixed bias could not do that. Non-vacuous: the "
            "pooled test resolves a coherent bias of %.3g relative (gate %.3g), the worst "
            "pixel %.3g (gate %.3g), and the smallest per-pixel sigma is %.3gx the "
            "arithmetic floor\n",
            cfg.checkNumber, kSeedsFull, kBounces, kCpuFull, cfg.name, kCapacity, worstZ[1],
            worstZPixel[1], kPixels, kPerPixelZ, pooledDetail[0], pooledDetail[1], pooledZ[1],
            kPooledZ, rmsPrefix, rmsFull, convergenceRatio, kConvergenceMin, kConvergenceMax,
            pooledResolution, kMaxPooledResolution, worstPixelResolution,
            kMaxPerPixelResolution,
            (sigmaFloor > 0.0) ? minSigma / sigmaFloor
                               : std::numeric_limits<double>::infinity());
    }
    return true;
}

}  // namespace ohao::diff::probe
