// The independent CPU reference integrator (Stage 0b-2b Task 6, checks
// 33-34). Lifted verbatim out of diff_gpu_probe.cpp: the same code with the
// same commentary, in its own translation unit. The types and the two ray
// constants it used to declare at file scope now live in
// oracle_integrator.hpp, because ties.cpp reads the constants across a
// module boundary -- a linkage change, not a value change.
#include "probe/oracle_integrator.hpp"

#include "gpu_probe_context.hpp"

#include <algorithm>
#include <cmath>

namespace ohao::diff::probe {

// ===========================================================================
// INDEPENDENT CPU REFERENCE INTEGRATOR (Stage 0b-2b Task 6, checks 33-34)
// ===========================================================================
//
// WHY THIS EXISTS AND NOT PathTracer. Task 6 asks for parity against
// ohao::PathTracer. PathTracer is a VK_KHR_ray_tracing_pipeline renderer:
// .rgen/.rmiss/.rchit stages, a shader binding table, ~35 descriptor
// bindings including storage images, a bindless texture array and an
// environment image. GpuProbeContext::init enables VK_KHR_ray_query and
// deliberately NOT VK_KHR_ray_tracing_pipeline (gpu_probe_context.cpp's
// device-extension list), and Task 6's constraints forbid adding a device
// feature or extension. So PathTracer cannot be constructed against this
// context at all, and this is a REPLACEMENT reference, chosen and stated as
// a design call rather than an omission. See the task report.
//
// WHAT MAKES IT AN ORACLE -- AND WHAT IS ACTUALLY SHARED, STATED PRECISELY.
// "Oracle" here does not mean "shares nothing with the GPU side"; it means
// every piece that IS shared is a piece some OTHER check has already
// independently validated, so nothing this check's own verdict depends on is
// taken on faith. What follows is exhaustive, not illustrative.
//
// INDEPENDENT (no code, no constant, no sampling machinery in common):
//   * its own ray-triangle intersector (Moller & Trumbore, "Fast, Minimum
//     Storage Ray/Triangle Intersection", JGT 2(1), 1997) instead of a BVH
//     traversal;
//   * its own orthonormal basis (Duff et al., "Building an Orthonormal Basis,
//     Revisited", JCGT 6(1), 2017) -- deliberately not oracleCosineHemisphere
//     above, whose frame convention is a transcription of the shader's;
//   * COSINE-HEMISPHERE sampling for the direct-lighting integral, with NO
//     environment importance sampling, NO CDF, NO pdfEnvMap and NO multiple
//     importance sampling. The GPU estimates the same integral by combining
//     an environment sample and a BSDF sample under the balance heuristic.
//     Two different estimators of one integral is the whole point: they agree
//     only if both are unbiased, so a wrong CDF, a wrong recovered radiance,
//     an MIS partition that does not sum to 1, or a double-counted throughput
//     all show up as a difference that does NOT shrink with sample count.
//   * its own RNG (std::mt19937_64, seeded per pixel) rather than
//     ohao::diff::PathRng.
//
// SHARED, AND WHY EACH ONE IS SAFE TO SHARE:
//   * INPUTS -- the scene triangles, the environment image and the camera.
//     Sharing the scene is what "the same scene" means; it is not sharing an
//     algorithm.
//   * THE BSDF, oracleBsdfEval above -- shared CODE, but not shared with the
//     GPU. Check 20 already pins bsdf.glsl against this exact oracle,
//     term by term, over its domain. Reusing it here reuses a validated
//     independent implementation; nothing in oracleBsdfEval was transcribed
//     from bsdf.glsl. See "WHAT THIS COSTS" below for what reusing it means
//     for checks 33-34's own scope.
//   * TWO CONSTANTS, transcribed from the shaders. ParityRefScene::rayTMax
//     (kParityRayTMax, 1000.0) mirrors TWO of them, one per kind of ray the
//     reference traces: wf_intersect.comp's `kTraceTMax` for the primary and
//     continuation trace, and wf_scatter.comp's `kShadowTMax` for the shadow
//     ray. ParityRefScene::surfaceOffset (kParitySurfaceOffset, 1e-4)
//     mirrors ONE, wf_scatter.comp's `kSurfaceOffset`, which that shader
//     applies to both the shadow ray's origin and the continuation ray's;
//     wf_intersect.comp has no offset of its own to mirror -- it traces with
//     tMin = 0 and derives its safety from this shader's offset instead (see
//     the derivation above its ray query). It has no `1e-4` constant of its
//     own; the only occurrence of that string in the file is the comment at
//     :158 explaining why there is none. All three shader constants are tied to these two C++
//     ones at runtime by checkParityRefConstantsTie, the same mechanism
//     checkNeeStrideTie and checkScatterPushSizeTie use for the constants
//     they cover.
//   * THE ENVIRONMENT DIRECTION<->TEXEL CONVENTION (parityEnvRadiance above),
//     the same inversion checks 25/27/31 pin the GPU's own recovered
//     radiance against, texel by texel. It is not independently re-derived
//     here; it is a validated host-side statement about the scene.
//   * THE GEOMETRIC-NORMAL CONSTRUCTION AND FLIP (parityReferenceSample's
//     `n = cross(e1,e2)` then oppose-the-ray), the same convention check 19
//     pins wf_intersect.comp's recovered normal against.
//
// WHAT THIS COSTS. Because checks 3/4/8, 19, 20 and 25/27/31 already
// establish that the pieces above are correct independently of this check,
// checks 33-34 are not testing them again -- in particular, since check 20
// already establishes bsdf.glsl equivalent to oracleBsdfEval over its
// domain, checks 33-34 do NOT test the BSDF; they test everything AROUND
// it (the intersector, the visibility term, the MIS combination, the
// throughput recursion, the film accumulation) under a BSDF both sides are
// known to agree on. That is the right decomposition -- it isolates what
// checks 33-34 alone can newly say -- but it makes them a CONDITIONAL gate,
// conditional on checks 3/4/8, 19, 20 and 25/27/31 all still passing, not
// the unconditional one a reader could mistake "own intersector, basis,
// RNG..." for.
//
// WHAT QUANTITY IS COMPUTED, EXACTLY. wf_scatter.comp's film holds
//
//     F(p) = SUM over bounces k of  T_k * Lhat_direct(x_k)
//
// -- the MIS-combined direct lighting from the environment at every surface
// vertex a path visits, carried by the throughput on ARRIVAL at that vertex,
// truncated at a fixed bounce count, with NO background term for a ray that
// escapes, and (as of Stage 1 Task 4) a uniform emissive-surface term gated
// by `pc.emission` -- left at its 0.0 default here, as in every check that
// predates Task 4, so it is absent from THIS comparison by construction,
// not because the pipeline has nothing to evaluate; see traverse.glsl's
// binding-9 Film doc for the general case and check 42 for the case where
// it is not zero. parityReferenceSample below computes exactly that: the
// same truncation, the same missing background term, and the same (zero)
// emission, so the two sides are comparable term for term.
//
// THE ESCAPE TERM, AND WHY THERE IS NO GAP HERE. Two different things get
// called "the missing escape term" and only one of them is real:
//
//   (a) A ray that escapes at bounce k >= 1 having been drawn from the BSDF.
//       This is NOT missing. wf_scatter.comp evaluates the BSDF strategy's
//       environment contribution EAGERLY at the vertex the ray leaves --
//       T_k * w_B * (f cos / p) * L_env(w_b) * V(w_b), where V is a shadow
//       ray along the very direction the path continues in -- and V is 1
//       exactly when that continuation ray would have escaped. A classical
//       NEE+MIS path tracer instead adds T_{k+1} * w_B * L_env at the miss,
//       and T_{k+1} = T_k * (f cos / p), so the two are the same number
//       written in two places. Dropping the escaped path from the next
//       bounce's queue afterwards is therefore correct, not lossy.
//   (b) The CAMERA ray missing every surface. This one IS absent from the
//       film: it has no preceding BSDF sample and so no MIS partner, and
//       wf_intersect.comp simply drops the path.
//
// The parity scene below is built so that (b) is IDENTICALLY EMPTY -- every
// primary ray hits the floor, far from any silhouette -- and the check
// MEASURES that rather than assuming it, by running one bounce and requiring
// all `capacity` paths to survive. So the compared quantity is not a
// restricted one on this scene; it is the whole image.

/// Moller & Trumbore 1997. `tMax` is wf_intersect.comp's / the shadow ray's
/// own trace limit; `t > 0` mirrors those rays' tMin of exactly 0 (see
/// wf_intersect.comp, which offsets the ORIGIN off the surface instead of
/// raising tMin).
bool parityRayTriangle(const OracleVec3& origin, const OracleVec3& dir, const ParityTriangle& tri,
                       double tMax, double& outT) {
    const OracleVec3 pv = oracleCross(dir, tri.e2);
    const double det = oracleDot(tri.e1, pv);
    if (std::abs(det) < 1e-14) return false;
    const double invDet = 1.0 / det;
    const OracleVec3 tv{origin.x - tri.v0.x, origin.y - tri.v0.y, origin.z - tri.v0.z};
    const double u = oracleDot(tv, pv) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    const OracleVec3 qv = oracleCross(tv, tri.e1);
    const double v = oracleDot(dir, qv) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    const double t = oracleDot(tri.e2, qv) * invDet;
    if (t <= 0.0 || t >= tMax) return false;
    outT = t;
    return true;
}

/// Nearest hit over the whole soup. Returns the triangle index or -1.
int parityTraceNearest(const std::vector<ParityTriangle>& tris, const OracleVec3& origin,
                       const OracleVec3& dir, double tMax, double& outT) {
    int best = -1;
    double bestT = tMax;
    for (std::size_t i = 0; i < tris.size(); ++i) {
        double t = 0.0;
        if (parityRayTriangle(origin, dir, tris[i], bestT, t)) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) outT = bestT;
    return best;
}

/// Any hit within tMax. This is the reference's visibility term; it mirrors
/// the SEMANTICS of wf_scatter.comp's diffShadowVisibility (terminate on the
/// first hit, environment reached iff nothing was hit before kShadowTMax),
/// not its implementation.
bool parityOccluded(const std::vector<ParityTriangle>& tris, const OracleVec3& origin,
                    const OracleVec3& dir, double tMax) {
    for (const ParityTriangle& tri : tris) {
        double t = 0.0;
        if (parityRayTriangle(origin, dir, tri, tMax, t)) return true;
    }
    return false;
}

/// Duff et al. 2017, branchless. Independent of oracleFrame and of
/// bsdf.glsl's own frame construction; any orthonormal frame gives the same
/// cosine distribution, so nothing about the estimator depends on which.
void parityBasis(const OracleVec3& n, OracleVec3& t, OracleVec3& b) {
    const double sign = std::copysign(1.0, n.z);
    const double a = -1.0 / (sign + n.z);
    const double bb = n.x * n.y * a;
    t = OracleVec3{1.0 + sign * n.x * n.x * a, sign * bb, -sign * n.x};
    b = OracleVec3{bb, sign + n.y * n.y * a, -n.y};
}

/// Malley's method: a cosine-weighted direction about `n`, pdf = cos/pi.
OracleVec3 parityCosineSample(const OracleVec3& n, double u1, double u2) {
    const double r = std::sqrt(u1);
    const double phi = 2.0 * kOraclePi * u2;
    const double x = r * std::cos(phi);
    const double y = r * std::sin(phi);
    const double z = std::sqrt(std::max(0.0, 1.0 - u1));
    OracleVec3 t, b;
    parityBasis(n, t, b);
    return oracleNormalize(oracleAdd(oracleAdd(oracleScale(t, x), oracleScale(b, y)),
                                     oracleScale(n, z)));
}

/// The equirectangular environment, piecewise constant per texel. Uses the
/// SAME host-side inversion checks 25/27/31 use to name the texel a
/// direction lands in -- literally the same function now, oracleEnvTexelOf,
/// rather than a fifth transcription of it -- and check 31 is what pins the
/// GPU's recovered radiance to this array texel by texel, so this lookup is a
/// validated host-side statement about the scene, not a copy of any shader.
double parityEnvRadiance(const OracleVec3& dir, const std::vector<double>& envLum, uint32_t envW,
                         uint32_t envH) {
    return envLum[oracleEnvTexelOf(dir.x, dir.y, dir.z, envW, envH).index];
}

/// A [0,1) uniform from the reference's own generator.
double parityNextU(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
}

/// ONE independent sample of the film's value at one pixel: the truncated
/// sum over surface vertices of throughput times a cosine-sampled,
/// MIS-free estimate of the direct lighting from the environment. Grey --
/// the scene's base colour and the environment are both grey, so the three
/// film channels are the same number (checks 33-34 assert that they are, bit
/// for bit, rather than assuming it).
double parityReferenceSample(const ParityRefScene& scene, const OracleVec3& camOrigin,
                             const OracleVec3& camDir, std::mt19937_64& rng) {
    OracleVec3 origin = camOrigin;
    OracleVec3 dir = camDir;
    double throughput = 1.0;  // wf_generate.comp writes (1,1,1).
    double total = 0.0;

    for (uint32_t k = 0; k < scene.bounces; ++k) {
        double t = 0.0;
        const int hit = parityTraceNearest(scene.tris, origin, dir, scene.rayTMax, t);
        if (hit < 0) break;  // Escaped. No background term -- see the header.

        const ParityTriangle& tri = scene.tris[static_cast<std::size_t>(hit)];
        OracleVec3 n = oracleNormalize(oracleCross(tri.e1, tri.e2));
        if (oracleDot(n, dir) > 0.0) n = oracleScale(n, -1.0);  // Oppose the ray.
        const OracleVec3 V = oracleScale(dir, -1.0);
        const OracleVec3 x{origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t};
        const OracleVec3 offsetOrigin{x.x + n.x * scene.surfaceOffset,
                                      x.y + n.y * scene.surfaceOffset,
                                      x.z + n.z * scene.surfaceOffset};

        // --- Direct lighting, cosine-sampled. The estimator is
        // f * cos / pdf * L_env * V with pdf = cos/pi, i.e. pi * f * L * V.
        {
            const OracleVec3 wi = parityCosineSample(n, parityNextU(rng), parityNextU(rng));
            OracleVec3 f;
            double pdfUnused = 0.0;
            oracleBsdfEval(n, V, wi, scene.material, f, pdfUnused);
            if (f.x > 0.0) {
                const bool blocked = parityOccluded(scene.tris, offsetOrigin, wi, scene.rayTMax);
                if (!blocked) {
                    total += throughput * kOraclePi * f.x *
                             parityEnvRadiance(wi, *scene.envLum, scene.envW, scene.envH);
                }
            }
        }

        // --- Continuation, cosine-sampled from an INDEPENDENT pair of
        // uniforms. Estimator weight f*cos/pdf = pi*f again.
        const OracleVec3 wo = parityCosineSample(n, parityNextU(rng), parityNextU(rng));
        OracleVec3 fc;
        double pdfC = 0.0;
        oracleBsdfEval(n, V, wo, scene.material, fc, pdfC);
        throughput *= kOraclePi * fc.x;
        if (!(throughput > 0.0)) break;  // Zero weight: nothing downstream can contribute.

        origin = offsetOrigin;
        dir = wo;
    }
    return total;
}

/// wf_generate.comp's primary ray, in double. Reproduces
/// shaders/includes/diff/camera_ray.glsl's construction -- pixel centre,
/// y-down NDC, aspect on the horizontal axis -- which checks 3, 4 and 8
/// already pin against closed-form distances and a half-quad orientation
/// test. The camera is an INPUT both integrators share; it is not part of
/// what this check is measuring.
OracleVec3 parityCameraRay(uint32_t px, uint32_t py, uint32_t width, uint32_t height,
                           const ohao::diff::WavefrontGenerateCamera& cam) {
    const double ndcX = 2.0 * (static_cast<double>(px) + 0.5) / static_cast<double>(width) - 1.0;
    const double ndcY = 1.0 - 2.0 * (static_cast<double>(py) + 0.5) / static_cast<double>(height);
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    const OracleVec3 f{cam.forward[0], cam.forward[1], cam.forward[2]};
    const OracleVec3 r{cam.right[0], cam.right[1], cam.right[2]};
    const OracleVec3 u{cam.up[0], cam.up[1], cam.up[2]};
    return oracleNormalize(oracleAdd(
        oracleAdd(f, oracleScale(r, ndcX * aspect * static_cast<double>(cam.tanHalfFov))),
        oracleScale(u, ndcY * static_cast<double>(cam.tanHalfFov))));
}

/// Appends one axis-aligned quad's two triangles, in the caller's vertex
/// order, to a positions/indices soup in runWavefrontIntersectOnGeometry's
/// packing. The winding is the CALLER'S: every parity-scene quad below is
/// wound so that wf_intersect.comp's flip-to-oppose-the-ray step actually
/// fires for the rays that reach it, which is what keeps a missing flip
/// observable rather than a no-op.
void parityAddQuad(std::vector<float>& positions, std::vector<uint32_t>& indices,
                   const std::array<std::array<float, 3>, 4>& corners) {
    const uint32_t base = static_cast<uint32_t>(positions.size() / 3u);
    for (const auto& c : corners) {
        positions.push_back(c[0]);
        positions.push_back(c[1]);
        positions.push_back(c[2]);
    }
    indices.push_back(base + 0u);
    indices.push_back(base + 1u);
    indices.push_back(base + 2u);
    indices.push_back(base + 0u);
    indices.push_back(base + 2u);
    indices.push_back(base + 3u);
}

/// The host mirror of the soup handed to the GPU: the SAME floats, turned
/// into the reference intersector's edge form. Built from the same arrays
/// that are uploaded, so the two sides cannot see different geometry.
std::vector<ParityTriangle> parityTrianglesFromSoup(const std::vector<float>& positions,
                                                    const std::vector<uint32_t>& indices) {
    std::vector<ParityTriangle> tris;
    tris.reserve(indices.size() / 3u);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto vertexAt = [&](uint32_t vi) {
            return OracleVec3{positions[static_cast<std::size_t>(vi) * 3u + 0u],
                              positions[static_cast<std::size_t>(vi) * 3u + 1u],
                              positions[static_cast<std::size_t>(vi) * 3u + 2u]};
        };
        const OracleVec3 a = vertexAt(indices[i + 0]);
        const OracleVec3 b = vertexAt(indices[i + 1]);
        const OracleVec3 c = vertexAt(indices[i + 2]);
        tris.push_back(ParityTriangle{a, {b.x - a.x, b.y - a.y, b.z - a.z},
                                      {c.x - a.x, c.y - a.y, c.z - a.z}});
    }
    return tris;
}

/// Welford-free two-pass moments over a contiguous prefix. Returns the mean
/// and the UNBIASED sample variance (n-1), which is what a standard error
/// needs.
void parityMoments(const double* values, std::size_t n, double& outMean, double& outVar) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += values[i];
    outMean = sum / static_cast<double>(n);
    double sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = values[i] - outMean;
        sq += d * d;
    }
    outVar = (n > 1) ? sq / static_cast<double>(n - 1) : 0.0;
}

/// The SAME statistic as parityMoments -- mean and unbiased sample variance
/// (n-1) -- from a running (sum, sum-of-squares) PAIR instead of a stored
/// array. This is what checks 33-34 use for the CPU reference's per-pixel
/// moments: cpuSumFull/cpuSumSqFull accumulate across kCpuFull samples per
/// pixel, per worker thread, without retaining each sample (retaining them
/// would be kPixels * kCpuFull doubles held live for no purpose parityMoments
/// could not already serve from two running totals), so there is no array
/// here for parityMoments' two-pass form to read. The GPU side's moments (in
/// the SAME loop, a few lines away) DO come from a stored array -- one film
/// per seed already exists -- so it goes through parityMoments directly; the
/// two forms differ because their INPUTS differ shape, not by choice.
///
/// The two forms are algebraically identical
/// (sum((x-mean)^2) == sum(x^2) - n*mean^2) and differ only in
/// floating-point ROUNDING: this one-pass identity can lose more of the
/// mantissa to cancellation than the two-pass form when sum(x^2) and
/// n*mean^2 are close. At this file's sample counts and value magnitudes
/// (order 1, up to kCpuFull = 4096 samples) that cancellation costs roughly 6
/// decimal digits of a double's ~16 -- immaterial against the check's own
/// tolerances, so nothing here is actually lost; this is a documented
/// difference, not an unexplained one.
void parityMomentsFromSums(double sum, double sumSq, std::size_t n, double& outMean,
                           double& outVar) {
    outMean = sum / static_cast<double>(n);
    outVar = (n > 1) ? std::max(0.0, (sumSq - static_cast<double>(n) * outMean * outMean) /
                                          static_cast<double>(n - 1))
                     : 0.0;
}

}  // namespace ohao::diff::probe
