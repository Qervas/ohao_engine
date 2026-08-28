// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention;
//      also exercises ohao::diff::ComputePipeline's build/bind/destroy
//      lifecycle directly (Stage 0b-2a Task 1), then replays the same
//      dispatch through ohao::diff::WavefrontStage::record() (Stage 0b-2a
//      Task 2) into an independent arena block, proving record() actually
//      drives bind/push/dispatch rather than silently no-op'ing.
//   3. rayQueryEXT visibility matches a closed-form plane intersection.
//   4. A half-quad's hit/miss pattern pins the camera's Y orientation --
//      check 3's distance formula is even in dy, so it can't catch a flipped
//      up-vector or NDC-Y sign on its own.
//   ...
//   8. wf_generate.comp's generated rays reproduce check 3's closed form.
//   9. Every PathStateField wf_generate.comp writes round-trips exactly.
//   10. wf_generate.comp's queue/counter population has no atomicAdd races.
//   11. path_state_layout.hpp and path_state.glsl agree field-by-field.
//   12. wf_intersect.comp compacts survivors of a known-fraction scene into
//       queue ring 1 exactly (no duplicates, no dead paths), across an
//       indirectly-sized dispatch prepared by wf_prepare_indirect.comp.
//   13. An indirect dispatch sized from a live-count of 0 launches zero
//       invocations -- dead paths are genuinely free.
//   14. wf_scatter.comp's constant-albedo throughput decay is exact after 4
//       bounces (p=0.5 -> 0.0625, compared bit-exact, no tolerance) --
//       proves the Throughput and Bounce fields survive 4 separate dispatch
//       boundaries intact. (Origin/Dir are written every bounce but not
//       exercised by any assertion here -- intersect runs once, not per
//       bounce, so positions after bounce 0 are geometrically meaningless
//       scaffolding; Stage 0b-2 running intersect per bounce is what would
//       make them worth checking.)
//   15. wf_scatter.comp's per-bounce RNG draws (values AND drawCount) match
//       ohao::diff::PathRng's replay exactly, for every one of those 4
//       dispatches -- extends check 6's single-stream parity guarantee
//       across dispatch boundaries, which is what path replay in Stage 1
//       depends on.
//   16-18. The SAME two analytic properties as checks 14-15, re-proved with
//       the whole bounce loop fused into ONE command buffer through
//       ohao::diff::WavefrontLoop -- no vkQueueWaitIdle between stages, so
//       ordering is the barriers' job rather than a full-device idle wait's.
//       16 also asserts every path survives every bounce and the live ring
//       holds each path index exactly once, which is what keeps 17 from
//       passing vacuously over an empty survivor set.
//   19. wf_intersect.comp's stored geometric normal equals the analytic
//       surface normal of the face it hit, for every path, against a closed
//       axis-aligned box whose face planes and outward winding are known
//       host-side. The oracle is that geometry plus the closed-form camera
//       ray -- nothing the shader computed.
//   20. The BSDF itself (shaders/includes/diff/bsdf.glsl, as called by
//       wf_scatter.comp and observed through bsdf_probe.comp): f, the
//       sampling pdf, and the sampler's f*cos/pdf weight all match a CPU
//       oracle written from Walter et al. 2007, Heitz 2014, Heitz 2018,
//       Schlick 1994 and PBRT -- not from the GLSL under test.
//   21. White furnace: albedo 1, no absorption, constant environment. The
//       cosine-sampled Lambert estimator is a constant 1 with ZERO variance,
//       so this is asserted to 4 ulp rather than to a statistical tolerance.
//       Catches energy-loss bugs in the whole sample-evaluate-weight loop
//       that a per-term comparison structurally cannot. Runs at q = 0
//       EXACTLY, which is the pure-Lambert early-return branch.
//   22. The same furnace with a white rough conductor: every path's weight
//       lies in [0,1] (provable pointwise -- it is G2/G1) and the mean is
//       strictly below 1, which is the known single-scattering GGX deficit
//       and NOT something that should be asserted to equal 1. Runs at
//       q = 1 EXACTLY.
//   23. The same furnace at an INTERMEDIATE lobe probability (q ~ 0.69), so
//       the mixture density and the f*cos/pdf division are executed at all
//       -- 21 and 22 sit at the two values of q that cannot bias anything,
//       and 21 does not even reach the division. Asserts a derived pointwise
//       energy-creation bound and a derived interval for the mean, the
//       latter anchored to 22's own measurement of the single-scattering
//       GGX albedo.
//   24. Environment importance sampling (shaders/includes/rt/env_sampling.glsl
//       as called by wf_scatter.comp): a Pearson chi-squared goodness-of-fit
//       test over 24576 directions a real GPU dispatch drew, binned into the
//       map's 128 texels, against an oracle computed here from the luminance
//       image and the analytic sin(theta) solid-angle weight -- NOT from the
//       CDF that was uploaded, so the host-side CDF builder is under test
//       too. The rejection threshold is derived (Wilson-Hilferty at
//       alpha = 1e-6), never tuned.
//   25. Every returned pdf is finite and strictly positive over a strictly
//       positive environment; the pdfs' range equals the map's own luminance
//       range, which is the sin(theta) Jacobian asserted as an equality
//       rather than as an integral; and every sampled direction inverts to a
//       texel CENTRE -- which is what makes 24's binning meaningful at all.
//   26. Those pdfs integrate to 1 over the sphere: pdf * dOmega is
//       condDiff * margDiff with the sine cancelling, so the sum is an
//       identity rather than a quadrature, and is asserted to a derived
//       float32 error budget.
//   27. ohao::diff::WavefrontLoop::record's OWN fill of ScatterPush's
//       envWidth/envHeight (production path -- checks 24-26 exercise
//       runWavefrontScatterProbe's hand-filled push constants, a different
//       call site) against a genuinely NON-SQUARE environment, read back
//       through the fused loop's binding-6 sink. The oracle is closed-form:
//       wf.build()'s default UV-uniform CDF makes pdf = 1/(2 pi^2 sin(theta))
//       exactly, so no CDF builder is needed to know what record() should
//       have produced.
//   32. RADIANCE ACCUMULATION INTO THE FILM (Stage 0b-2b Task 5). After a
//       FUSED multi-bounce run through WavefrontLoop::record, the
//       caller-owned film buffer equals the sum of the per-bounce
//       contributions, reconstructed on the host from primitives the
//       binding-7 sink records separately (arrival throughput, both
//       single-strategy estimators, both MIS weights, the pixel index) --
//       so the accumulator is never compared against a copy of itself. The
//       per-bounce values come from the probe's existing one-run-per-bounce
//       structure. Asserted to a derived gamma_{k+4} float32 bound, with
//       explicit non-vacuity gates on how far a dropped bounce would move
//       the film relative to that bound.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

// ohao::EnvCDF is the RT pipeline's own environment CDF builder. Check 24
// uploads what IT produces rather than re-deriving the CDF here, so the two
// pipelines cannot drift apart on the convention
// shaders/includes/rt/env_sampling.glsl assumes.
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <string>
#include <vector>

namespace {

// ===========================================================================
// INDEPENDENT CPU BSDF ORACLE (Stage 0b-2b Task 2, check 20)
// ===========================================================================
//
// WHERE THESE FORMULAS COME FROM. Every expression below was written from
// the published source cited immediately above it, in double precision, and
// NOT transcribed from shaders/includes/diff/bsdf.glsl or
// shaders/includes/material/ggx_aniso.glsl -- which is the whole point. A
// CPU "oracle" that is a line-by-line port of the GLSL under test agrees
// with it by construction and cannot fail; this project has already shipped
// six checks of that shape (Stage 0b-1), one of which compared throughput
// against pow(albedo, 4) computed from the very constant being perturbed.
//
//   D   -- Walter, Marschner, Li & Torrance, "Microfacet Models for
//          Refraction through Rough Surfaces", EGSR 2007, Eq. 33 (GGX /
//          Trowbridge-Reitz). Written in the paper's own tan-form,
//          D = a^2 / (pi cos^4(t_m) (a^2 + tan^2(t_m))^2), which is a
//          textually different expression from the
//          ((n.h)^2(a^2-1)+1)^2 form ggx_aniso.glsl uses. They are
//          algebraically equal -- that equality is part of what this check
//          tests.
//   Lambda, G1, G2
//       -- Heitz, "Understanding the Masking-Shadowing Function in
//          Microfacet-Based BRDFs", JCGT 3(2), 2014: Lambda for GGX Eq. 72,
//          G1 = 1/(1+Lambda) Eq. 43, height-correlated
//          G2 = 1/(1+Lambda_o+Lambda_i) Eq. 99.
//   F   -- Schlick, "An Inexpensive BRDF Model for Physically-based
//          Rendering", Computer Graphics Forum 13(3), 1994:
//          F = F0 + (1-F0)(1-cos)^5.
//   f_s -- Cook & Torrance 1982; Walter et al. 2007 Eq. 20:
//          f_s = D F G / (4 |n.i| |n.o|).
//   f_d, diffuse pdf
//       -- Lambert: f_d = rho/pi. Cosine-weighted hemisphere pdf =
//          cos(theta)/pi (Malley's method). Pharr, Jakob & Humphreys,
//          "Physically Based Rendering", 4th ed., Sec. 9.2 and A.5.3.
//   specular pdf
//       -- Heitz, "Sampling the GGX Distribution of Visible Normals",
//          JCGT 7(4), 2018, Eq. 3: D_V(m) = G1(o) max(0, o.m) D(m) / (o.n),
//          divided by the reflection Jacobian 4 (o.m) (Walter et al. 2007
//          Eq. 14): pdf(i) = G1(o) D(m) / (4 (o.n)).
//   F0 for the metal-rough parameterisation
//       -- Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 course
//          notes: F0 = mix(0.04, baseColor, metallic).
//
// The ONLY things below that are not from a paper are the lobe-selection
// probability `q` and the dielectric specular scale, because those are a
// SAMPLING STRATEGY and a material parameterisation, not physics -- there is
// no published formula to compare them against. They are stated as a
// contract in shaders/includes/diff/bsdf.glsl's header comment, and this
// oracle implements that stated contract independently. What that means for
// this check's strength: the `f` comparison is entirely paper-derived and
// cannot agree by construction, while the `pdf` comparison additionally
// pins the documented strategy.
//
// WHERE q IS ACTUALLY GUARDED, stated precisely because it is easy to
// overclaim. Three things reach it, and none of them is "the furnace covers
// it":
//   * check 20's pdf and weight comparisons, which use this oracle's q at
//     the host-chosen and GPU-sampled directions;
//   * check 20's branch-agreement assertion, which decides from the
//     DIRECTION THE GPU RETURNED which of the sampler's two lobes drew it
//     and requires that to match `uLobe < q`;
//   * check 23, the intermediate-q furnace run.
// Checks 21 and 22 do NOT cover q. Check 21 runs {roughness 1, metallic 0,
// specularWeight 0}, which is q = 0 exactly, and at q = 0 diffBsdfSample
// takes an early-return branch that never calls diffBsdfEval, never forms
// f or pdf and never divides -- so it verifies that one multiplication
// returns baseColor, not that f*cos/pdf is assembled correctly. Check 22
// runs a conductor, which is q = 1 exactly. Those are precisely the two
// values at which q cannot bias anything, which is why check 23 exists.

constexpr double kOraclePi = 3.14159265358979323846;

struct OracleVec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

OracleVec3 oracleAdd(const OracleVec3& a, const OracleVec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
OracleVec3 oracleScale(const OracleVec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double oracleDot(const OracleVec3& a, const OracleVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
OracleVec3 oracleCross(const OracleVec3& a, const OracleVec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
OracleVec3 oracleNormalize(const OracleVec3& a) {
    const double len = std::sqrt(oracleDot(a, a));
    return len > 0.0 ? oracleScale(a, 1.0 / len) : OracleVec3{0.0, 0.0, 1.0};
}

// Walter et al. 2007, Eq. 33, in the paper's tan-form.
double oracleGgxD(double NdotH, double alpha) {
    if (NdotH <= 0.0) return 0.0;
    const double cos2 = NdotH * NdotH;
    const double tan2 = (1.0 - cos2) / cos2;
    const double a2 = alpha * alpha;
    const double s = a2 + tan2;
    return a2 / (kOraclePi * cos2 * cos2 * s * s);
}

// Heitz 2014, Eq. 72.
double oracleSmithLambda(double cosTheta, double alpha) {
    if (cosTheta <= 0.0) return 0.0;
    const double cos2 = cosTheta * cosTheta;
    const double tan2 = (1.0 - cos2) / cos2;
    return 0.5 * (std::sqrt(1.0 + alpha * alpha * tan2) - 1.0);
}

// Heitz 2014, Eq. 43.
double oracleSmithG1(double cosTheta, double alpha) {
    return 1.0 / (1.0 + oracleSmithLambda(cosTheta, alpha));
}

// Heitz 2014, Eq. 99 (height-correlated).
double oracleSmithG2(double NdotV, double NdotL, double alpha) {
    return 1.0 / (1.0 + oracleSmithLambda(NdotV, alpha) + oracleSmithLambda(NdotL, alpha));
}

// Schlick 1994.
double oracleSchlick(double f0, double cosTheta) {
    const double m = std::max(0.0, 1.0 - cosTheta);
    const double m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

/// The material exactly as bsdf.glsl's header states it.
struct OracleMaterial {
    OracleVec3 baseColor{1.0, 1.0, 1.0};
    double roughness{1.0};
    double metallic{0.0};
    double specularWeight{0.0};
};

OracleVec3 oracleF0(const OracleMaterial& m) {
    return {0.04 + (m.baseColor.x - 0.04) * m.metallic,
            0.04 + (m.baseColor.y - 0.04) * m.metallic,
            0.04 + (m.baseColor.z - 0.04) * m.metallic};
}

/// Contract, not physics: metals always carry a full specular lobe;
/// dielectrics scale theirs by specularWeight.
double oracleSpecScale(const OracleMaterial& m) {
    return m.specularWeight + (1.0 - m.specularWeight) * m.metallic;
}

/// Contract, not physics: the lobe-selection probability.
double oracleSpecProb(const OracleMaterial& m, double NdotV) {
    const OracleVec3 f0 = oracleF0(m);
    const double cosI = std::min(1.0, std::abs(NdotV));
    const double fr = std::max(oracleSchlick(f0.x, cosI),
                               std::max(oracleSchlick(f0.y, cosI), oracleSchlick(f0.z, cosI)));
    double q = oracleSpecScale(m) * fr * (1.0 - m.roughness * 0.9);
    q = q + (1.0 - q) * m.metallic;
    return std::min(1.0, std::max(0.0, q));
}

/// f(N, V, L) and the pdf of the documented sampling strategy at L.
void oracleBsdfEval(const OracleVec3& N, const OracleVec3& V, const OracleVec3& L,
                    const OracleMaterial& m, OracleVec3& outF, double& outPdf) {
    outF = {0.0, 0.0, 0.0};
    outPdf = 0.0;

    const double NdotL = oracleDot(N, L);
    const double NdotV = oracleDot(N, V);
    if (NdotL <= 0.0 || NdotV <= 0.0) return;

    // Lambert. Metals have no diffuse lobe.
    const double kd = 1.0 - m.metallic;
    outF = oracleScale(m.baseColor, kd / kOraclePi);
    outPdf = NdotL / kOraclePi;

    const double q = oracleSpecProb(m, NdotV);
    if (q <= 0.0) return;

    const double alpha = m.roughness * m.roughness;
    const OracleVec3 H = oracleNormalize(oracleAdd(V, L));
    const double NdotH = std::max(0.0, oracleDot(N, H));
    const double VdotH = std::max(0.0, oracleDot(V, H));

    const double D = oracleGgxD(NdotH, alpha);
    const double G2 = oracleSmithG2(NdotV, NdotL, alpha);
    const OracleVec3 f0 = oracleF0(m);
    const double common = oracleSpecScale(m) * D * G2 / (4.0 * NdotV * NdotL);

    outF.x += common * oracleSchlick(f0.x, VdotH);
    outF.y += common * oracleSchlick(f0.y, VdotH);
    outF.z += common * oracleSchlick(f0.z, VdotH);

    const double pdfSpec = oracleSmithG1(NdotV, alpha) * D / (4.0 * NdotV);
    outPdf = outPdf * (1.0 - q) + pdfSpec * q;
}

/// Directional albedo, INT f(N,V,L) (N.L) dL over the upper hemisphere, by
/// midpoint quadrature of THIS FILE'S oracle f. It is independent of the
/// GLSL under test in exactly the way oracleBsdfEval is: the published model
/// integrated numerically, with nothing the GPU produced entering it.
///
/// This is precisely the quantity a furnace estimates. With a constant
/// environment L0 = 1, an unbiased single-sample estimator has
/// E[f*cos/pdf] = INT f cos dL = rho_dir(V), whatever the sampling strategy
/// is -- so comparing the GPU's sample mean against this number is a direct
/// statement about the whole sample-evaluate-weight loop, at a lobe
/// probability that is neither 0 nor 1.
///
/// The BSDF is isotropic, so only the angle between N and V matters; N is
/// taken as +Z and V placed in the x-z plane at the requested cosine. The
/// materials this is called with are grey, so the red channel is the whole
/// answer.
double oracleDirectionalAlbedo(const OracleMaterial& m, double cosThetaV, uint32_t nTheta,
                               uint32_t nPhi) {
    const OracleVec3 N{0.0, 0.0, 1.0};
    const double sinThetaV = std::sqrt(std::max(0.0, 1.0 - cosThetaV * cosThetaV));
    const OracleVec3 V{sinThetaV, 0.0, cosThetaV};
    const double dTheta = (0.5 * kOraclePi) / static_cast<double>(nTheta);
    const double dPhi = (2.0 * kOraclePi) / static_cast<double>(nPhi);
    double total = 0.0;
    for (uint32_t i = 0; i < nTheta; ++i) {
        const double theta = (static_cast<double>(i) + 0.5) * dTheta;
        const double st = std::sin(theta);
        const double ct = std::cos(theta);
        for (uint32_t j = 0; j < nPhi; ++j) {
            const double phi = (static_cast<double>(j) + 0.5) * dPhi;
            const OracleVec3 L{st * std::cos(phi), st * std::sin(phi), ct};
            OracleVec3 f;
            double pdf = 0.0;
            oracleBsdfEval(N, V, L, m, f, pdf);
            total += f.x * ct * st * dTheta * dPhi;
        }
    }
    return total;
}

/// Orthonormal frame around `n`, host-side, used only to place the probe's
/// V and L at chosen polar angles. Nothing the shader computes enters here.
void oracleFrame(const OracleVec3& n, OracleVec3& t, OracleVec3& b) {
    const OracleVec3 up =
        (std::abs(n.y) < 0.9) ? OracleVec3{0.0, 1.0, 0.0} : OracleVec3{1.0, 0.0, 0.0};
    t = oracleNormalize(oracleCross(up, n));
    b = oracleCross(n, t);
}

OracleVec3 oracleDirFromAngles(const OracleVec3& n, double theta, double phi) {
    OracleVec3 t, b;
    oracleFrame(n, t, b);
    const double st = std::sin(theta);
    return oracleNormalize(oracleAdd(oracleAdd(oracleScale(t, st * std::cos(phi)),
                                               oracleScale(b, st * std::sin(phi))),
                                     oracleScale(n, std::cos(theta))));
}

/// NOT part of the oracle, and never used as a reference value. This
/// reproduces bsdf.glsl's diffCosineHemisphere (Malley's method, with that
/// function's documented up-vector convention) for exactly one purpose:
/// deciding, FROM THE DIRECTION THE GPU RETURNED, which of diffBsdfSample's
/// two branches drew it. The two branches draw from different distributions,
/// so the direction identifies the branch -- which is what makes the
/// both-lobes-exercised guard below a measurement of GPU behaviour rather
/// than a restatement of the hardcoded material table.
OracleVec3 oracleCosineHemisphere(const OracleVec3& n, double u1, double u2) {
    const double r = std::sqrt(u1);
    const double phi = 2.0 * kOraclePi * u2;
    const double x = r * std::cos(phi);
    const double y = r * std::sin(phi);
    const double z = std::sqrt(std::max(0.0, 1.0 - u1));
    const OracleVec3 up =
        (std::abs(n.z) < 0.999) ? OracleVec3{0.0, 0.0, 1.0} : OracleVec3{1.0, 0.0, 0.0};
    const OracleVec3 t = oracleNormalize(oracleCross(up, n));
    const OracleVec3 b = oracleCross(n, t);
    return oracleNormalize(oracleAdd(oracleAdd(oracleScale(t, x), oracleScale(b, y)),
                                     oracleScale(n, z)));
}

double oracleDistance(const OracleVec3& a, const OracleVec3& b) {
    const OracleVec3 d{a.x - b.x, a.y - b.y, a.z - b.z};
    return std::sqrt(oracleDot(d, d));
}

/// bsdf.glsl's DIFF_BSDF_MIN_COS. The shader treats a view or light cosine at
/// or below this as grazing and refuses the specular math; this oracle
/// rejects at 0, because the physics does. The two thresholds are therefore
/// NOT the same, and the band (0, 1e-4] is a documented disagreement rather
/// than a bug -- see the rejection branch below, which names it explicitly
/// instead of letting it surface as a spurious failure.
constexpr double kShaderGrazingCos = 1e-4;

/// Relative difference that degrades gracefully to absolute near zero. f and
/// pdf span many orders of magnitude across the case table (a sharp GGX
/// lobe's D is ~10^3 at roughness 0.1 and ~10^-1 at roughness 0.8), so a
/// purely absolute tolerance would be meaningless at one end and vacuous at
/// the other.
double oracleRelDiff(double reference, double measured) {
    const double denom = std::max(1e-6, std::abs(reference));
    return std::abs(measured - reference) / denom;
}

/// Bind ohao::diff::kNeeSampleFloats to wf_scatter.comp's OWN
/// kNeeSampleFloats, at runtime, by reading the declaration out of the
/// shader source.
///
/// WHY A RUNTIME CHECK AND NOT A static_assert. The two constants live on
/// opposite sides of the GLSL/C++ boundary. GLSL has no static_assert; the
/// value is folded into unnamed SPIR-V literals, so it cannot be reflected
/// out of the compiled module under a name either; and there is no
/// generated header in this build that both sides could include. What is
/// left is the source text, which is authoritative because it is what the
/// shader compiler consumed. A mismatch is a SILENT wrong-slot read -- the
/// host would stride the readback by one number while the GPU wrote it with
/// another, producing plausible-looking garbage rather than a validation
/// error -- so this fails the whole probe rather than warning.
///
/// The search climbs parent directories looking for the SOURCE tree. Note
/// this is NOT the same mechanism as ComputePipeline::loadSpv, which
/// enumerates fixed sibling roots (bin/shaders/, build/Release/bin/shaders/)
/// for compiled SPV BINARIES -- an earlier comment here claimed they matched
/// and they do not. Not finding the source is itself a failure, because "the
/// tie could not be checked" must not be allowed to read as "the tie holds".
///
/// The declaration is looked for in a COMMENT-STRIPPED copy of the source,
/// not the raw text. Scanning raw text would match a commented-out
/// declaration -- `// const uint kNeeSampleFloats = 21u;` -- and report a
/// live, tied constant even if the real one had been renamed or deleted.
/// Commenting a declaration out is precisely how such a constant goes
/// missing, so the one case this check exists to catch is the one raw
/// scanning would miss.
///
/// Shared by checkNeeStrideTie and checkScatterPushSizeTie below: both are
/// GLSL/C++ ties against this same source file, and both need the same
/// comment-stripping for the same reason (see the previous paragraph), so
/// this is the one place that logic lives rather than two copies that could
/// drift apart from each other.
bool loadWfScatterSourceStripped(std::string& outStripped, std::string& outFoundPath) {
    static const char* const kCandidates[] = {
        "shaders/diff/wf_scatter.comp",
        "../shaders/diff/wf_scatter.comp",
        "../../shaders/diff/wf_scatter.comp",
        "../../../shaders/diff/wf_scatter.comp",
    };
    std::string text;
    const char* found = nullptr;
    for (const char* candidate : kCandidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (!in.is_open()) continue;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        found = candidate;
        break;
    }
    if (found == nullptr) return false;
    outFoundPath = found;

    // Strip GLSL comments before matching. GLSL has no string literals, so a
    // two-state scan over // and /* */ is exact here. Newlines are preserved
    // so that a regex's [ \t] (which cannot span lines) still rejects a
    // reformatted multi-line declaration.
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') stripped.push_back('\n');
                ++i;
            }
            i = (i + 1 < text.size()) ? i + 2 : text.size();
        } else {
            stripped.push_back(text[i]);
            ++i;
        }
    }
    outStripped = std::move(stripped);
    return true;
}

bool checkNeeStrideTie() {
    std::string stripped, found;
    if (!loadWfScatterSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/diff/wf_scatter.comp from any "
                     "candidate path, so the binding-7 record stride could not be tied to "
                     "ohao::diff::kNeeSampleFloats (%u). An unchecked tie is not a held tie: a "
                     "mismatch is a silent wrong-slot read, not a validation error\n",
                     ohao::diff::kNeeSampleFloats);
        return false;
    }

    const std::regex decl(R"(const[ \t]+uint[ \t]+kNeeSampleFloats[ \t]*=[ \t]*([0-9]+)u[ \t]*;)");
    std::smatch m;
    if (!std::regex_search(stripped, m, decl)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `const uint kNeeSampleFloats = "
                     "<N>u;` on one line, so the binding-7 record stride cannot be tied to "
                     "ohao::diff::kNeeSampleFloats (%u). Restore the spelling or update this "
                     "check -- do not leave the two constants untied\n",
                     found.c_str(), ohao::diff::kNeeSampleFloats);
        return false;
    }
    const unsigned long shaderStride = std::stoul(m[1].str());
    if (shaderStride != static_cast<unsigned long>(ohao::diff::kNeeSampleFloats)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s writes %lu floats per path into binding 7 while "
                     "ohao::diff::kNeeSampleFloats says %u. The host sizes the buffer and strides "
                     "the readback by its own number, so every slot past path 0 would be read at "
                     "the wrong offset and every check below would be measuring the wrong "
                     "floats\n",
                     found.c_str(), shaderStride, ohao::diff::kNeeSampleFloats);
        return false;
    }
    std::printf("[diff_gpu_probe] NOTE: binding-7 record stride tied -- %s declares %lu floats per "
                "path and ohao::diff::kNeeSampleFloats is %u\n",
                found.c_str(), shaderStride, ohao::diff::kNeeSampleFloats);
    return true;
}

/// Ties wf_scatter.comp's `Push` block byte size to
/// `ohao::diff::WavefrontLoop::ScatterPush`'s, the same way checkNeeStrideTie
/// ties the NEE record stride: by parsing the SHADER SOURCE (comment-stripped
/// via loadWfScatterSourceStripped, for the identical reason) rather than
/// trusting a comment.
///
/// WHY THIS EXISTS (review Finding 6, Stage 0b-2b Task 5). ScatterPush has no
/// static_assert and no runtime tie today, and it has grown a tail field in
/// each of Tasks 2 (material), 3 (environment) and 5 (film) -- three chances
/// for the C++ struct and the GLSL block to drift, caught so far only by two
/// humans reading two files side by side. kNeeSampleFloats got a runtime tie
/// precisely because "naming each other in a comment was not enough"; the
/// same argument applies here with equal force, so this reuses that
/// mechanism rather than settling for a static_assert against a hand-copied
/// literal, which would tie ScatterPush to a DOCUMENTED number but not to the
/// shader itself -- it would not notice a field added to the GLSL block
/// without a matching C++ change, only the reverse.
///
/// HOW THE BYTE COUNT IS COMPUTED. Every field the Push block has ever had is
/// a bare scalar `uint` or `float` -- no vec/mat/array member exists in it --
/// so each one is 4 bytes with no interior padding, and the block's size is
/// simply 4 * (field count). That assumption is exactly the kind of thing
/// that could silently stop holding, so it is checked rather than baked in:
/// the field-matching regex's count is cross-checked against a plain
/// semicolon count over the same block, and a mismatch (e.g. a vec3 field,
/// which this regex does not match) fails loudly instead of mis-sizing.
bool checkScatterPushSizeTie() {
    std::string stripped, found;
    if (!loadWfScatterSourceStripped(stripped, found)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: could not open shaders/diff/wf_scatter.comp from any "
                     "candidate path, so its Push block size could not be tied to "
                     "ohao::diff::WavefrontLoop::ScatterPush. An unchecked tie is not a held "
                     "tie\n");
        return false;
    }

    const std::string beginMarker = "layout(push_constant) uniform Push {";
    const std::size_t beginPos = stripped.find(beginMarker);
    if (beginPos == std::string::npos) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s no longer declares `layout(push_constant) uniform "
                     "Push { ... } pc;` in the exact spelling this check looks for, so its size "
                     "cannot be tied to ScatterPush. Restore the spelling or update this check -- "
                     "do not leave the two untied\n",
                     found.c_str());
        return false;
    }
    const std::size_t blockStart = beginPos + beginMarker.size();
    const std::size_t endPos = stripped.find("} pc;", blockStart);
    if (endPos == std::string::npos) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block has no matching `} pc;` this check "
                     "can find, so its size cannot be tied to ScatterPush\n",
                     found.c_str());
        return false;
    }
    const std::string block = stripped.substr(blockStart, endPos - blockStart);

    const std::regex fieldRe(R"((?:uint|float)[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*;)");
    const std::size_t fieldCount = static_cast<std::size_t>(
        std::distance(std::sregex_iterator(block.begin(), block.end(), fieldRe),
                      std::sregex_iterator()));
    const std::size_t semicolons =
        static_cast<std::size_t>(std::count(block.begin(), block.end(), ';'));
    if (semicolons != fieldCount || fieldCount == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block has %zu statement(s) but only %zu "
                     "matched a bare `uint`/`float` scalar field. This check's byte-size math (4 "
                     "bytes/field, no padding) assumes every field is one of those two scalar "
                     "types; a vec/array/other-typed field would silently mis-size instead of "
                     "being counted correctly. Update this check to handle it rather than "
                     "trusting the mismatch away\n",
                     found.c_str(), semicolons, fieldCount);
        return false;
    }

    const std::size_t shaderBytes = fieldCount * 4u;
    constexpr std::size_t kScatterPushBytes = sizeof(ohao::diff::WavefrontLoop::ScatterPush);
    if (shaderBytes != kScatterPushBytes) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: %s's Push block is %zu scalar fields (%zu bytes) but "
                     "sizeof(ohao::diff::WavefrontLoop::ScatterPush) is %zu bytes. "
                     "WavefrontLoop::record fills that struct and vkCmdPushConstants pushes it "
                     "byte-for-byte as this shader's push constants; a size mismatch is a silent "
                     "wrong-field push, not a validation error, and every field after the point "
                     "of drift is read at the wrong offset\n",
                     found.c_str(), fieldCount, shaderBytes, kScatterPushBytes);
        return false;
    }
    std::printf("[diff_gpu_probe] NOTE: wf_scatter.comp Push block tied to ScatterPush -- %s "
                "declares %zu scalar fields (%zu bytes) and sizeof(ScatterPush) is %zu\n",
                found.c_str(), fieldCount, shaderBytes, kScatterPushBytes);
    return true;
}

}  // namespace

int main() {
    // The GLSL/C++ record-stride tie, before any Vulkan object exists: if it
    // does not hold, nothing measured below means anything.
    if (!checkNeeStrideTie()) return 1;
    // The GLSL/C++ ScatterPush byte-size tie (review Finding 6, Task 5): same
    // reasoning, same failure mode (a silent wrong-field push), checked here
    // for the same "before anything downstream trusts it" reason.
    if (!checkScatterPushSizeTie()) return 1;

    ohao::diff::GpuProbeContext ctx;
    if (!ctx.init()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: could not initialise Vulkan\n");
        return 1;
    }

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(ctx.physicalDevice());
    if (!caps.sufficient()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: device lacks ray query or float atomics\n");
        return 1;
    }

    ohao::diff::ArenaLayout layout;
    const std::size_t blockA = layout.add(16);
    const std::size_t blockB = layout.add(4);
    // Reserved for the WavefrontStage check below (Stage 0b-2a Task 2) so it
    // has its own independent target index rather than reusing blockA's,
    // which check 2 has already mutated by the time this runs -- reusing it
    // would make this check's result depend on execution order.
    const std::size_t blockC = layout.add(16);

    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), layout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build\n");
        return 1;
    }

    // 1. zero + readback
    ctx.runImmediate([&](VkCommandBuffer cmd) { arena.zero(cmd); });
    for (std::size_t b : {blockA, blockB}) {
        const std::vector<float> values = arena.readback(ctx.allocator(), b);
        if (values.empty()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned no data "
                         "(this check would otherwise pass having verified nothing)\n",
                         b);
            return 1;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: block %zu element %zu = %f, expected 0\n",
                             b, i, values[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: arena zero + readback\n");

    // 2. float atomics under contention. Also exercises
    // ohao::diff::ComputePipeline (Task 1, Stage 0b-2a) directly: build()
    // for diff_atomic_probe.comp.spv, confirm pipeline()/layout()/
    // descriptorSet() are all non-null, bind the arena buffer, then call
    // destroy() twice to confirm the second call is a no-op and that
    // destroy() nulls all three handles.
    //
    // This has its own OK line. It used to have none, on the stated grounds
    // that folding it into the atomics check below "keeps this section at
    // one printed OK line, matching the pre-refactor count" -- which is
    // exactly backwards: two of the assertions here (a second destroy() is
    // a no-op; destroy() nulls pipeline()/layout()/descriptorSet()) are
    // covered by nothing else, so deleting them would leave this probe's
    // stdout byte-identical. A check whose removal is invisible is not a
    // check. Printed OK-line counts are an output of what is verified,
    // never a target to hold constant.
    {
        ohao::diff::ComputePipeline pipelineSanity;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        if (!pipelineSanity.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                                  /*pushConstantSize=*/8)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::build\n");
            return 1;
        }
        if (pipelineSanity.pipeline() == VK_NULL_HANDLE ||
            pipelineSanity.layout() == VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline built a null handle\n");
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!pipelineSanity.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::bindBuffers\n");
            return 1;
        }
        pipelineSanity.destroy(ctx.device());
        pipelineSanity.destroy(ctx.device());  // must be a no-op, not a double-free
        if (pipelineSanity.pipeline() != VK_NULL_HANDLE ||
            pipelineSanity.layout() != VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() != VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::destroy left a handle live\n");
            return 1;
        }
    }
    std::printf("[diff_gpu_probe] OK: ComputePipeline build + bind + double destroy (second "
                "destroy is a no-op, all handles nulled)\n");

    constexpr uint32_t kInvocations = 4096;
    if (!ctx.runAtomicProbe(arena, /*targetIndex=*/0, kInvocations)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomic probe dispatch\n");
        return 1;
    }
    const std::vector<float> after = arena.readback(ctx.allocator(), blockA);
    if (after.size() < 2) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected "
                     "at least 2 (a readback regression here would otherwise be undefined "
                     "behaviour, not a caught failure)\n",
                     blockA, after.size());
        return 1;
    }
    if (after[0] != static_cast<float>(kInvocations)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: atomicAdd gave %f, expected %u "
                     "(lost updates = non-atomic accumulation)\n",
                     after[0], kInvocations);
        return 1;
    }
    if (after[1] != 0.0f) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomicAdd wrote outside target index\n");
        return 1;
    }
    std::printf("[diff_gpu_probe] OK: atomicAdd accumulated %u contended adds exactly\n",
                kInvocations);

    // 2b. ohao::diff::WavefrontStage (Stage 0b-2a Task 2): drives the same
    // atomic-probe canary as check 2, but through record() -- bind
    // pipeline, bind descriptor set, push constants, vkCmdDispatch -- rather
    // than a hand-rolled sequence. This is a real differential, not just a
    // handle-non-null sanity check: if record() forgot to bind the
    // descriptor set, pushed the wrong constants, or dispatched a Fixed
    // group count of 0, the canary would land on something other than
    // exactly kStageInvocations (most likely 0, since block C was zeroed
    // alongside every other block in check 1 and nothing else writes to
    // it), not silently pass.
    constexpr uint32_t kStageInvocations = 2048;
    {
        ohao::diff::WavefrontStage stage;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        struct PushConstants {
            uint32_t targetIndex;
            uint32_t invocationCount;
        };
        if (!stage.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                         sizeof(PushConstants))) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::build\n");
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!stage.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::bindBuffers\n");
            return 1;
        }

        // Absolute float index of block C within the arena's single flat
        // `data[]` array -- computed from the layout rather than hardcoded,
        // since blockC's byte offset depends on blockA/blockB's sizes.
        const ohao::diff::ArenaBlock blockCInfo = layout.block(blockC);
        const uint32_t targetIndex = static_cast<uint32_t>(blockCInfo.offsetBytes / sizeof(float));

        const PushConstants push{targetIndex, kStageInvocations};
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(
            ohao::diff::WavefrontStage::Fixed{(kStageInvocations + 63u) / 64u});

        ctx.runImmediate([&](VkCommandBuffer cmd) {
            stage.record(cmd);

            // Same host-read barrier dispatchStorageBufferCompute records
            // after its own dispatch -- that function, in
            // gpu_probe_context.cpp, IS the reference sequence for this
            // pattern (compute_pipeline.cpp records no barriers at all; it
            // contains no vkCmdPipelineBarrier call) --
            // WavefrontStage::record() deliberately does not
            // add this itself (see its class comment), so the check adds it
            // directly, exactly as WavefrontLoop will for each stage it
            // sequences in Task 3.
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = arena.buffer();
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        });
        stage.destroy(ctx.device());

        const std::vector<float> stageResult = arena.readback(ctx.allocator(), blockC);
        if (stageResult.empty()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned no data\n", blockC);
            return 1;
        }
        if (stageResult[0] != static_cast<float>(kStageInvocations)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: WavefrontStage::record produced %f, expected "
                         "%u contended adds (stage did not run, or ran the wrong invocation "
                         "count)\n",
                         stageResult[0], kStageInvocations);
            return 1;
        }
        for (std::size_t i = 1; i < stageResult.size(); ++i) {
            if (stageResult[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: WavefrontStage::record wrote outside its "
                             "target index (block %zu element %zu = %f)\n",
                             blockC, i, stageResult[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: WavefrontStage::record replays the atomic-probe canary -- "
                "%u contended adds exactly through a Fixed dispatch\n", kStageInvocations);

    // 3. Ray-query visibility against a plane whose intersections are analytic.
    //
    // kW != kH deliberately: a square resolution makes the closed form
    // (even in both dx and dy, see check 4's comment) blind to a transposed
    // pixel index, and leaves the ndcX*aspect term in the shader untested
    // since aspect == 1 would hide a missing/backwards aspect multiply.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;  // narrow, so every ray hits the quad
    constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
    // Widened horizontal extent still lands inside the +-1 quad:
    // kTanHalfFov * kAspect ~= 0.267, so max |x| at z=-2 is ~0.53.

    std::vector<float> hitsT;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, hitsT)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: visibility probe dispatch\n");
        return 1;
    }
    if (hitsT.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: hit buffer size %zu, expected %u\n",
                     hitsT.size(), kW * kH);
        return 1;
    }

    // The centre pixel looks straight down -Z, so t is exactly the plane distance.
    const std::size_t centre = static_cast<std::size_t>(kH / 2) * kW + (kW / 2);
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-4f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return 1;
    }

    // Off-axis rays are longer by exactly 1/cos(theta), and every ray must hit.
    // Tolerance is 1e-4: float32 at t~=2 supports ~1e-5, so this still leaves
    // ~10x headroom over float noise while catching e.g. a dropped +0.5
    // pixel-centre offset (worst-case error ~1.19e-3 at this FOV, comfortably
    // above 1e-4). Do not widen this to make a failure pass -- report the
    // actual max error and stop.
    float maxAbsError = 0.0f;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            if (hitsT[i] < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) missed a quad that "
                             "covers the whole frustum\n", x, y);
                return 1;
            }
            const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
            const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
            const float dx = ndcX * kAspect * kTanHalfFov;
            const float dy = ndcY * kTanHalfFov;
            const float expected = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
            const float err = std::fabs(hitsT[i] - expected);
            maxAbsError = std::max(maxAbsError, err);
            if (err > 1e-4f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) t = %f, closed form %f "
                             "(|err| = %g)\n",
                             x, y, hitsT[i], expected, err);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: ray query matches closed-form plane intersection "
                "over %u pixels (max |err| = %g)\n", kW * kH, maxAbsError);

    // 4. A half-quad (y in [0,1] only) makes the hit/miss pattern asymmetric
    // in Y. The closed form sqrt(1+dx^2+dy^2) above is even in dy, so a
    // flipped up-vector or a flipped NDC-Y sign is invisible to it -- every
    // ray still lands at the "right" distance from *some* consistent-looking
    // convention, even a backwards one. This check pins the actual convention
    // Stage 0b's integrator inherits: ndcY > 0 for y < kH/2, i.e. the top
    // half of the image must hit and the bottom half must miss.
    std::vector<float> halfHits;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, halfHits,
                                /*quadMinY=*/0.0f)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad visibility probe dispatch\n");
        return 1;
    }
    if (halfHits.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad hit buffer size %zu, expected %u\n",
                     halfHits.size(), kW * kH);
        return 1;
    }
    for (uint32_t y = 0; y < kH; ++y) {
        const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
        const bool expectHit = (ndcY > 0.0f);
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            const bool didHit = (halfHits[i] >= 0.0f);
            if (didHit != expectHit) {
                std::fprintf(stderr,
                    "[diff_gpu_probe] FAIL: half-quad orientation at (%u,%u): expected %s, got %s "
                    "(camera up-vector or NDC-Y sign is inverted)\n",
                    x, y, expectHit ? "hit" : "miss", didHit ? "hit" : "miss");
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: half-quad pins camera Y orientation over %u pixels\n",
                kW * kH);

    // 4. The seam Stage 1 depends on most: a block index handed out by the registry
    //    must resolve correctly against an arena built from that registry's layout.
    //    Both hold ArenaLayout by value, so this proves the positional indices survive
    //    the copy -- previously true only by inspection.
    {
        ohao::diff::ParamRegistry reg;
        const auto reg1 = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
        const auto reg2 = reg.registerScalarBlock("ssao_params", 4);
        if (!reg1.ok || !reg2.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registry setup: %s %s\n",
                         reg1.error.c_str(), reg2.error.c_str());
            return 1;
        }

        ohao::diff::GradientArena regArena;
        if (!regArena.build(ctx.allocator(), reg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build from registry layout\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { regArena.zero(cmd); });

        const ohao::diff::DiffParam* albedo = reg.find("albedo");
        const ohao::diff::DiffParam* ssao = reg.find("ssao_params");
        if (albedo == nullptr || ssao == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registered params not found\n");
            regArena.destroy(ctx.allocator());
            return 1;
        }

        struct Expect { const char* name; std::size_t block; std::size_t floats; };
        const Expect expects[] = {
            {"albedo.grad",  albedo->gradBlock,  albedo->floatCount},
            {"albedo.state", albedo->stateBlock, albedo->floatCount * 2u},
            {"ssao.grad",    ssao->gradBlock,    ssao->floatCount},
            {"ssao.state",   ssao->stateBlock,   ssao->floatCount * 2u},
        };
        for (const Expect& e : expects) {
            const std::vector<float> block = regArena.readback(ctx.allocator(), e.block);
            if (block.size() != e.floats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: %s resolved to %zu floats, registry says %zu "
                             "(registry/arena block indices disagree)\n",
                             e.name, block.size(), e.floats);
                regArena.destroy(ctx.allocator());
                return 1;
            }
            for (float v : block) {
                if (v != 0.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s not zeroed\n", e.name);
                    regArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        regArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: registry block indices resolve against arena "
                    "(4 blocks, sizes match)\n");
    }


    // 6. CPU/GPU RNG parity -- the invariant path replay backpropagation rests on.
    //    The backward pass stores no tape; it replays each light path from its seed.
    //    If shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp disagree by
    //    a single bit, the replayed path is a DIFFERENT path and every gradient is
    //    silently wrong -- no crash, no NaN. Until now the two were verified
    //    identical only by reading them side by side.
    {
        constexpr uint32_t kDraws = 64;
        constexpr uint32_t kPixel = 4096;
        constexpr uint32_t kSample = 3;
        constexpr uint32_t kSeed = 12345;

        ohao::diff::ArenaLayout rngLayout;
        const std::size_t drawBlock = rngLayout.add(kDraws);
        ohao::diff::GradientArena rngArena;
        if (!rngArena.build(ctx.allocator(), rngLayout)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng arena build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { rngArena.zero(cmd); });

        std::vector<float> gpuDraws;
        if (!ctx.runRngParityProbe(kPixel, kSample, kSeed, kDraws, rngArena, drawBlock, gpuDraws)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng parity dispatch\n");
            rngArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::PathRng cpuRng = ohao::diff::PathRng::forPath(kPixel, kSample, kSeed);
        for (uint32_t i = 0; i < kDraws; ++i) {
            const float cpu = cpuRng.next1D();
            const float gpu = gpuDraws[i];
            // Bit-exact on purpose: an epsilon here would defeat the entire check.
            if (cpu != gpu) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: RNG diverges at draw %u: CPU %.9g, GPU %.9g\n"
                             "  shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp must\n"
                             "  produce identical sequences -- path replay depends on it\n",
                             i, static_cast<double>(cpu), static_cast<double>(gpu));
                rngArena.destroy(ctx.allocator());
                return 1;
            }
        }
        if (cpuRng.drawCount() != kDraws) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: drawCount %u, expected %u\n",
                         cpuRng.drawCount(), kDraws);
            rngArena.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: CPU PathRng drawCount matches expected count (%u)\n",
                    kDraws);
        rngArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: CPU and GLSL RNG agree bit-exactly over %u draws\n",
                    kDraws);
    }

    // 7. Wavefront path-state/queue/counter buffers: build, zero, and confirm
    //    every one of the PathStateFields (all kFieldCount of them, whatever
    //    that count is today) and both counter slots come back zeroed at
    //    exactly the expected element count. This is the
    //    substrate every bounce dispatch in the wavefront integrator reads
    //    and writes through -- state cannot live in registers across a
    //    dispatch boundary.
    {
        constexpr std::uint32_t kCapacity = 4096;
        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wavefront buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        for (std::uint32_t i = 0; i < ohao::diff::PathStateLayout::kFieldCount; ++i) {
            const auto field = static_cast<ohao::diff::PathStateField>(i);
            const std::vector<float> values = wf.readbackField(ctx.allocator(), field);
            // Explicit size check (not just empty()) guards against a vacuous
            // pass: a readback regression that silently truncated to a
            // smaller-but-nonempty vector would otherwise loop over fewer
            // elements than the field actually has and pass having verified
            // less than it claims.
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: field %u readback returned %zu elements, "
                             "expected %u\n",
                             i, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            for (std::size_t e = 0; e < values.size(); ++e) {
                if (values[e] != 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: field %u element %zu = %f, expected 0\n",
                                 i, e, values[e]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        for (std::uint32_t slot : {ohao::diff::WavefrontBuffers::kCurrentCountSlot,
                                    ohao::diff::WavefrontBuffers::kNextCountSlot}) {
            const std::uint32_t count = wf.readbackCounter(ctx.allocator(), slot);
            if (count != 0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: counter slot %u = %u, expected 0\n",
                             slot, count);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: wavefront buffers zero across %u fields x %u paths "
                    "and both counter slots\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 8-10. Wavefront generate stage (shaders/diff/wf_generate.comp): one
    // dispatch, three checks against its output.
    //   8.  Closed form: generated directions reproduce the same hit
    //       distances check 3 validated analytically -- wf_generate.comp
    //       builds rays through camera_ray.glsl, the same include
    //       visibility_probe.comp uses, so this is what catches the two
    //       drifting apart.
    //   9.  Field round-trip: every PathStateField the shader wrote reads
    //       back on the CPU as what was written -- proves path_state.glsl
    //       and path_state_layout.hpp agree on the arena's byte layout, the
    //       same failure mode rng.glsl had (compiled, never executed).
    //   10. Queue population: the counter equals the pixel count exactly and
    //       queue 0 contains each path index exactly once -- an atomicAdd
    //       race would show up as duplicates or a short count.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Default camera: origin (0,0,0), forward (0,0,-1), right (1,0,0),
        // up (0,1,0) -- the same convention checks 3/4's runVisibilityProbe
        // calls use.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;

        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> originX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginX);
        const std::vector<float> originY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginY);
        const std::vector<float> originZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginZ);
        const std::vector<float> dirX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirX);
        const std::vector<float> dirY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirY);
        const std::vector<float> dirZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirZ);
        const std::vector<float> throughputR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        const std::vector<float> radianceR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceR);
        const std::vector<float> radianceG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceG);
        const std::vector<float> radianceB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceB);
        const std::vector<float> pixelIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::PixelIndex);
        const std::vector<float> sampleIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::SampleIndex);
        const std::vector<float> bounceField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Bounce);
        const std::vector<float> aliveField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);

        // Deliberately NOT all kFieldCount fields: wf_generate.comp does not
        // write HitT (that is wf_intersect.comp's output, checked
        // separately in check 12), so this list -- and the count printed
        // below, which is derived from its size rather than kFieldCount --
        // covers exactly what this stage actually produces.
        const std::vector<const std::vector<float>*> allFields = {
            &originX, &originY, &originZ, &dirX, &dirY, &dirZ,
            &throughputR, &throughputG, &throughputB,
            &radianceR, &radianceG, &radianceB,
            &pixelIndexField, &sampleIndexField, &bounceField, &aliveField,
        };
        for (const auto* f : allFields) {
            if (f->size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_generate field readback size %zu, "
                             "expected %u\n",
                             f->size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        // 8. Closed form.
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                const float dx = ndcX * kAspect * kTanHalfFov;
                const float dy = ndcY * kTanHalfFov;
                const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);

                // origin=(0,0,0), plane at z=-planeDistance: t = -planeDistance / dir.z.
                // dir.z < 0 is guaranteed at this FOV, same as check 3.
                const float dz = dirZ[i];
                if (dz >= 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated dir.z = %f, "
                                 "expected < 0\n",
                                 x, y, dz);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                const float actualT = -kPlaneDistance / dz;
                const float err = std::fabs(actualT - expectedT);
                maxAbsError = std::max(maxAbsError, err);
                if (err > 1e-4f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated-dir t = %f, "
                                 "closed form %f (|err| = %g)\n",
                                 x, y, actualT, expectedT, err);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_generate directions reproduce closed-form plane "
                    "intersection over %u pixels (max |err| = %g)\n",
                    kCapacity, maxAbsError);

        // 9. Field round-trip.
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (originX[i] != camera.origin[0] || originY[i] != camera.origin[1] ||
                originZ[i] != camera.origin[2]) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u origin = (%f,%f,%f), expected "
                             "(%f,%f,%f)\n",
                             i, originX[i], originY[i], originZ[i], camera.origin[0],
                             camera.origin[1], camera.origin[2]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (throughputR[i] != 1.0f || throughputG[i] != 1.0f || throughputB[i] != 1.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%f,%f,%f), expected "
                             "(1,1,1)\n",
                             i, throughputR[i], throughputG[i], throughputB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (radianceR[i] != 0.0f || radianceG[i] != 0.0f || radianceB[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u radiance = (%f,%f,%f), expected "
                             "(0,0,0)\n",
                             i, radianceR[i], radianceG[i], radianceB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }

            uint32_t pixelIndexBits = 0, sampleIndexBits = 0, bounceBits = 0, aliveBits = 0;
            std::memcpy(&pixelIndexBits, &pixelIndexField[i], sizeof(pixelIndexBits));
            std::memcpy(&sampleIndexBits, &sampleIndexField[i], sizeof(sampleIndexBits));
            std::memcpy(&bounceBits, &bounceField[i], sizeof(bounceBits));
            std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));

            if (pixelIndexBits != i) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u pixelIndex = %u, expected %u\n",
                             i, pixelIndexBits, i);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (sampleIndexBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u sampleIndex = %u, expected 0\n",
                             i, sampleIndexBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (bounceBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u bounce = %u, expected 0\n", i,
                             bounceBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (aliveBits != 1u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u alive = %u, expected 1\n", i,
                             aliveBits);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // Dir's exact components were already validated via the
            // closed-form distance check above; here just confirm it
            // round-tripped as a finite, unit-length vector -- catches e.g.
            // an offset error that silently aliased into the wrong field's
            // block without disturbing the z-only distance check.
            const float len2 = dirX[i] * dirX[i] + dirY[i] * dirY[i] + dirZ[i] * dirZ[i];
            if (std::fabs(len2 - 1.0f) > 1e-3f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u dir not unit length: |dir|^2 = %f\n",
                             i, len2);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: all %zu wf_generate-written PathStateFields round-trip "
                    "across %u paths (origin, dir, throughput=1, radiance=0, pixelIndex, "
                    "sampleIndex=0, bounce=0, alive=1)\n",
                    allFields.size(), kCapacity);

        // 10. Queue population.
        const std::uint32_t counter =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCurrentCountSlot);
        if (counter != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 counter = %u, expected %u\n",
                         counter, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        if (queue0.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 readback size %zu, expected %u\n",
                         queue0.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sorted = queue0;
        std::sort(sorted.begin(), sorted.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sorted[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue 0 sorted[%u] = %u, expected %u "
                             "(atomicAdd race: duplicate or missing path index)\n",
                             i, sorted[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: queue 0 counter = %u and contains each of %u path "
                    "indices exactly once\n",
                    counter, kCapacity);

        wf.destroy(ctx.allocator());
    }

    // 11. Layout-mapping probe (shaders/diff/wf_layout_probe.comp): proves
    // PathStateField enum order and path_state.glsl's PS_* constants agree,
    // field by field, in a way check 9 cannot. Check 9's values are
    // genuinely degenerate (origin (0,0,0), throughput (1,1,1), radiance
    // (0,0,0), sampleIndex and bounce both 0), so a transposition *within*
    // {OriginX,Y,Z}, within {ThroughputR,G,B}, within {RadianceR,G,B}, or
    // between SampleIndex and Bounce would round-trip there undetected.
    // Here every field gets a distinct value (1000+fieldIndex for floats,
    // 7000+fieldIndex for ints), so any permutation of the mapping mismatches
    // somewhere. Capacity 1000 is deliberately not a multiple of 64, so the
    // alignUp(capacity, 64) rounding path -- proven correct on paper for
    // capacity=1000 during review -- is exercised by actual execution here,
    // not just by the other checks' capacities (4096, all multiples of 64).
    {
        constexpr std::uint32_t kCapacity = 1000;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        if (!ctx.runWavefrontLayoutProbe(wf)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        struct FieldExpect {
            ohao::diff::PathStateField field;
            const char* name;
            bool isInt;
            float expectedFloat;
            uint32_t expectedBits;
        };
        const FieldExpect expects[] = {
            {ohao::diff::PathStateField::OriginX, "OriginX", false, 1000.0f, 0u},
            {ohao::diff::PathStateField::OriginY, "OriginY", false, 1001.0f, 0u},
            {ohao::diff::PathStateField::OriginZ, "OriginZ", false, 1002.0f, 0u},
            {ohao::diff::PathStateField::DirX, "DirX", false, 1003.0f, 0u},
            {ohao::diff::PathStateField::DirY, "DirY", false, 1004.0f, 0u},
            {ohao::diff::PathStateField::DirZ, "DirZ", false, 1005.0f, 0u},
            {ohao::diff::PathStateField::ThroughputR, "ThroughputR", false, 1006.0f, 0u},
            {ohao::diff::PathStateField::ThroughputG, "ThroughputG", false, 1007.0f, 0u},
            {ohao::diff::PathStateField::ThroughputB, "ThroughputB", false, 1008.0f, 0u},
            {ohao::diff::PathStateField::RadianceR, "RadianceR", false, 1009.0f, 0u},
            {ohao::diff::PathStateField::RadianceG, "RadianceG", false, 1010.0f, 0u},
            {ohao::diff::PathStateField::RadianceB, "RadianceB", false, 1011.0f, 0u},
            {ohao::diff::PathStateField::PixelIndex, "PixelIndex", true, 0.0f, 7012u},
            {ohao::diff::PathStateField::SampleIndex, "SampleIndex", true, 0.0f, 7013u},
            {ohao::diff::PathStateField::Bounce, "Bounce", true, 0.0f, 7014u},
            {ohao::diff::PathStateField::Alive, "Alive", true, 0.0f, 7015u},
            {ohao::diff::PathStateField::HitT, "HitT", false, 1016.0f, 0u},
            {ohao::diff::PathStateField::NormalX, "NormalX", false, 1017.0f, 0u},
            {ohao::diff::PathStateField::NormalY, "NormalY", false, 1018.0f, 0u},
            {ohao::diff::PathStateField::NormalZ, "NormalZ", false, 1019.0f, 0u},
        };
        // This array must cover every PathStateField or the "all %u
        // PathStateFields" claim below is false -- exactly the coverage gap
        // a fix round caught here once already (HitT was added to the enum
        // in Task 5 but not to this array or to wf_layout_probe.comp until a
        // review found the mismatch). A build-time check so a future field
        // addition fails loudly here instead of silently narrowing what
        // "all" means.
        static_assert(sizeof(expects) / sizeof(expects[0]) ==
                          ohao::diff::PathStateLayout::kFieldCount,
                      "expects[] must have exactly kFieldCount entries -- add the new field's "
                      "row here AND its psSet* write in wf_layout_probe.comp");

        for (const FieldExpect& e : expects) {
            const std::vector<float> values = wf.readbackField(ctx.allocator(), e.field);
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: layout probe field %s readback size %zu, "
                             "expected %u\n",
                             e.name, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (e.isInt) {
                uint32_t bits = 0;
                std::memcpy(&bits, &values[0], sizeof(bits));
                if (bits != e.expectedBits) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %u, expected %u "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, bits, e.expectedBits);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            } else {
                if (values[0] != e.expectedFloat) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %f, expected %f "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, values[0], e.expectedFloat);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: layout probe -- all %u PathStateFields hold their "
                    "distinct expected value at capacity=%u (non-multiple-of-64)\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 12. Wavefront intersect stage (shaders/diff/wf_intersect.comp) with
    // compaction, driven through an indirect dispatch prepared by
    // shaders/diff/wf_prepare_indirect.comp. This is the check that catches
    // an atomicAdd compaction race: it works because, for this specific
    // scene, the surviving set is knowable analytically rather than just
    // measured.
    //
    // Reuses check 4's half-quad (quadMinY=0.0): only rays with ndcY > 0
    // hit, i.e. pixel rows y < kH/2. Because pixelIndex = y*kW+x (row-major,
    // same as wf_generate.comp), that set is exactly the contiguous range
    // [0, kW*kH/2) = [0, 1536) -- so queue ring 1, sorted, must equal
    // 0..1535 with nothing else, and counter slot kNextCountSlot must read
    // exactly 1536.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr uint32_t kExpectedSurvivors = kCapacity / 2;  // 1536
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Populate queue ring 0 / counter slot 0 first, as its own fully
        // queue-idle-separated submission (see runWavefrontGenerateProbe) --
        // this is not the barrier under test, so it stays maximally safe.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // wf_prepare_indirect.comp + the indirectly-dispatched wf_intersect.comp,
        // all on one command buffer with the SHADER_WRITE -> INDIRECT_COMMAND_READ
        // barrier between them -- see gpu_probe_context.cpp's
        // runWavefrontIntersectProbe for exactly which barriers this records.
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != kExpectedSurvivors) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: queue ring 1 counter = %u, expected exactly %u "
                         "(compaction race: lost or duplicated an atomicAdd offset)\n",
                         nextCount, kExpectedSurvivors);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The canary must equal exactly the number of invocations the
        // indirect dispatch launched. kCapacity (3072) is an exact multiple
        // of wf_intersect.comp's local_size_x=64, so
        // groupCountX = kCapacity/64 with no rounding tail, and every
        // invocation runs (queue ring 0 holds all kCapacity paths). A wrong
        // kCanarySlot or a wrong push field would still leave check 13's
        // "canary == 0 on an empty queue" true, so that check alone cannot
        // catch this; asserting a specific *nonzero* expected value here is
        // what turns the canary into a real differential rather than a check
        // that only ever needs to prove absence.
        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected exactly %u invocations "
                         "(wrong kCanarySlot or push field would read 0 here and only be caught "
                         "by this nonzero assertion, not check 13's empty-queue one)\n",
                         canary, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        if (queue1.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue ring 1 readback size %zu, expected %u\n",
                         queue1.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The written prefix, sorted, must be exactly [0, kExpectedSurvivors)
        // with no duplicates and nothing extra.
        std::vector<uint32_t> survivors(queue1.begin(), queue1.begin() + kExpectedSurvivors);
        std::sort(survivors.begin(), survivors.end());
        for (uint32_t i = 0; i < kExpectedSurvivors; ++i) {
            if (survivors[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 sorted[%u] = %u, expected %u "
                             "(compaction race: duplicate or missing path index)\n",
                             i, survivors[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // The tail beyond what compaction actually wrote, [kExpectedSurvivors,
        // kCapacity), must still read as wf.zero()'s initial fill (0) --
        // never inspected until now. This is a "nothing extra was written"
        // sanity check, not a live exercise of the dstSlot < capacity guard
        // itself: at kCapacity=3072 with kExpectedSurvivors bounding dstSlot
        // well under capacity, that guard is provably unreachable in this
        // configuration. It still catches an off-by-one that spills the
        // compacted prefix past kExpectedSurvivors, a class the sorted-prefix
        // check above cannot see since that check only ever looks at
        // [0, kExpectedSurvivors).
        for (uint32_t i = kExpectedSurvivors; i < kCapacity; ++i) {
            if (queue1[i] != 0u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 tail[%u] = %u, expected 0 "
                             "(a write landed past the compacted prefix -- possible unclamped "
                             "dstSlot)\n",
                             i, queue1[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect compacted exactly %u survivors into queue "
                    "ring 1 (indices 0..%u, no duplicates, no dead paths, tail untouched)\n",
                    kExpectedSurvivors, kExpectedSurvivors - 1);

        // Bonus correctness beyond the brief's minimum: every survivor's
        // Alive flag stayed 1 and HitT matches check 8's closed form; every
        // dead path's Alive flag was cleared and HitT reads -1 (miss).
        const std::vector<float> aliveField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);
        const std::vector<float> hitTField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (aliveField.size() != kCapacity || hitTField.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_intersect Alive/HitT readback size mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                uint32_t aliveBits = 0;
                std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));
                const bool expectAlive = i < kExpectedSurvivors;
                if (static_cast<bool>(aliveBits) != expectAlive) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: path %u Alive = %u, expected %u\n",
                                 i, aliveBits, expectAlive ? 1u : 0u);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                if (expectAlive) {
                    const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                    const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                    const float dx = ndcX * kAspect * kTanHalfFov;
                    const float dy = ndcY * kTanHalfFov;
                    const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
                    const float err = std::fabs(hitTField[i] - expectedT);
                    maxAbsError = std::max(maxAbsError, err);
                    if (err > 1e-4f) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: path %u HitT = %f, closed form %f "
                                     "(|err| = %g)\n",
                                     i, hitTField[i], expectedT, err);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else if (hitTField[i] != -1.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u (dead) HitT = %f, expected -1\n",
                                 i, hitTField[i]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_intersect Alive/HitT match compaction membership and "
                    "closed-form plane intersection (max |err| = %g)\n", maxAbsError);

        wf.destroy(ctx.allocator());
    }

    // 13. Indirect dispatch of an empty queue costs nothing -- the property
    // that makes a dead path genuinely free rather than just skipped-but-
    // still-paid-for. Counter slot kCurrentCountSlot is left at 0 (no
    // wf_generate call), so wf_prepare_indirect.comp computes
    // groupCountX = (0+63)/64 = 0, and vkCmdDispatchIndirect with
    // groupCountX == 0 launches zero workgroups -- wf_intersect.comp's very
    // first instruction, an unconditional atomicAdd on a canary counter,
    // never executes. A stage that merely early-returned per-invocation
    // (rather than never launching) would still show this canary at 0, so
    // the real claim under test is the dispatch cost, not just the output --
    // this check is a necessary but not sufficient witness of that; it is
    // the best a black-box GPU probe can observe without a timing query.
    {
        constexpr uint32_t kCapacity = 64;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });
        // Deliberately no wf_generate call: counter slot kCurrentCountSlot
        // stays at 0 from wf.zero(), which is exactly the input under test.

        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, /*planeDistance=*/2.0f, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected 0 -- an indirect dispatch "
                         "sized from a live-count of 0 ran %u invocation(s)\n",
                         canary, canary);
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != 0) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: next-queue counter = %u, expected 0\n",
                         nextCount);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: indirect dispatch from a live-count of 0 ran zero "
                    "invocations (canary = 0, next-queue counter = 0)\n");

        wf.destroy(ctx.allocator());
    }

    // 14-15. Wavefront scatter stage (shaders/diff/wf_scatter.comp), run for
    // 4 real bounces: generate -> intersect once (full quad, quadMinY=-1, so
    // every ray hits and seeds every path into scatter) -> scatter x4,
    // ping-ponging the SAME two physical queue rings (each scatter call
    // zeroes its own destination counter slot first -- see
    // GpuProbeContext::runWavefrontScatterProbe's doc comment for why that
    // is load-bearing once a ring/slot pair is reused).
    //
    // Check 14 is the analytic throughput check: with albedo p=0.5 and every
    // ray surviving every bounce, throughput after 4 bounces must be exactly
    // p^4 = 0.0625, compared with ==, not a tolerance. This proves Throughput
    // and Bounce survive 4 dispatch boundaries -- it says nothing about
    // Origin/Dir, which this loop's single intersect call leaves stale after
    // bounce 0 (see the file header's note on check 14 for why).
    //
    // Check 15 is the RNG-parity check: for one chosen path, the exact
    // (u1, u2, drawCount) wf_scatter.comp computed at each of the 4 real,
    // separate dispatches must match ohao::diff::PathRng::forPath(...)
    // replayed the same number of draws on the CPU -- proving the GPU
    // reconstructs the RNG from (pixelIndex, sampleIndex, bounce) correctly
    // across a dispatch boundary, not just within a single dispatch (which
    // check 6 already covers).
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        // Must match wf_scatter.comp's kDrawsPerBounce. It became 3 in Stage
        // 0b-2b Task 2 (the BSDF draws a 2-D direction sample AND a 1-D lobe
        // choice every bounce) and 5 in Task 3 (two more for the environment
        // importance sample). The two VALUES compared below are unchanged
        // through both: the shader still draws the direction sample FIRST and
        // appends new draws after the old ones, so the debug sink still
        // records the same two stream positions -- only the per-bounce stride
        // moved, which is precisely what this constant exists to pin. Get it
        // wrong and the fast-forward in the shader and the replay here walk
        // different streams, which is the failure this check exists for.
        constexpr uint32_t kDrawsPerBounce = 5;
        constexpr uint32_t kChosenPath = 1234;   // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Full quad (quadMinY=-1.0): every ray hits, so every one of
        // kCapacity paths survives into scatter ring1/slot(kNextCountSlot).
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t seededCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (seededCount != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_scatter setup: %u of %u rays hit the full "
                         "quad, expected all of them (quadMinY=-1 should guarantee every ray "
                         "hits)\n",
                         seededCount, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Ping-pong: scatter's first source is the ring/slot wf_intersect
        // just filled (ring1/kNextCountSlot); its destination is the other
        // ring (ring0/kCurrentCountSlot), which currently holds a stale
        // pre-intersect count and gets zeroed by runWavefrontScatterProbe.
        uint32_t srcQueueBase = kCapacity;  // ring 1
        uint32_t srcCountSlot = ohao::diff::WavefrontBuffers::kNextCountSlot;
        uint32_t dstQueueBase = 0;  // ring 0
        uint32_t dstCountSlot = ohao::diff::WavefrontBuffers::kCurrentCountSlot;

        std::vector<std::vector<float>> drawsPerBounce(kBounces);
        bool scatterOk = true;
        for (uint32_t b = 0; b < kBounces && scatterOk; ++b) {
            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            if (!ctx.runWavefrontScatterProbe(wf, srcQueueBase, srcCountSlot, dstQueueBase,
                                              dstCountSlot, kAlbedo, kIterationSeed, outQueue,
                                              outDraws)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter dispatch at bounce %u\n", b);
                scatterOk = false;
                break;
            }
            const std::uint32_t survCount = wf.readbackCounter(ctx.allocator(), dstCountSlot);
            if (survCount != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued %u paths, "
                             "expected all %u (every invocation must re-queue -- nothing "
                             "terminates in scatter yet)\n",
                             b, survCount, kCapacity);
                scatterOk = false;
                break;
            }
            // Inspect the re-queued ring itself, mirroring check 12's
            // sorted-prefix inspection of wf_intersect's compaction output:
            // survCount alone is a single scalar and cannot distinguish
            // "every path re-queued exactly once" from "some path's slot
            // got overwritten while another was dropped, but the atomicAdd
            // count still landed on kCapacity by coincidence." This is
            // precisely the ring GpuProbeContext::runWavefrontScatterProbe
            // was already paying to copy back as outQueue -- it was going
            // unread before this check existed, which is exactly the blind
            // spot that let wf_scatter.comp ship without wf_intersect.comp's
            // dstSlot < capacity guard (Important 1): a missing bounds guard
            // changes what lands in this ring without necessarily changing
            // the counter atomicAdd returns.
            if (outQueue.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring readback "
                             "size %zu, expected %u\n",
                             b, outQueue.size(), kCapacity);
                scatterOk = false;
                break;
            }
            std::vector<uint32_t> sortedQueue = outQueue;
            std::sort(sortedQueue.begin(), sortedQueue.end());
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (sortedQueue[i] != i) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring "
                                 "sorted[%u] = %u, expected %u (duplicate or dead path index -- "
                                 "every survivor must appear exactly once)\n",
                                 b, i, sortedQueue[i], i);
                    scatterOk = false;
                    break;
                }
            }
            if (!scatterOk) break;
            if (outDraws.size() != static_cast<std::size_t>(kCapacity) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u debug-draws readback "
                             "size %zu, expected %zu\n",
                             b, outDraws.size(), static_cast<std::size_t>(kCapacity) * 3u);
                scatterOk = false;
                break;
            }
            drawsPerBounce[b] = std::move(outDraws);
            std::swap(srcQueueBase, dstQueueBase);
            std::swap(srcCountSlot, dstCountSlot);
        }
        if (!scatterOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter re-queued ring contains each of %u path "
                    "indices exactly once at every one of %u bounces (no duplicates, no dead "
                    "paths)\n",
                    kCapacity, kBounces);

        // 14. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f rather than computed as kAlbedo^kBounces here: if
        // this were derived from kAlbedo, perturbing kAlbedo alone (as
        // task-6-report.md's required proof does) would perturb BOTH sides
        // of the comparison identically and the check could never fail --
        // exactly the kind of tautological check the brief warns about. The
        // expected value must come from an independent source (arithmetic
        // done by hand: 0.5*0.5*0.5*0.5 = 0.0625), not from the GLSL/CPU
        // path currently under test.
        constexpr float expectedThroughput = 0.0625f;

        const std::vector<float> throughputR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (throughputR.size() != kCapacity || throughputG.size() != kCapacity ||
            throughputB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32, so an epsilon here would
            // defeat the entire point of the check.
            if (throughputR[i] != expectedThroughput || throughputG[i] != expectedThroughput ||
                throughputB[i] != expectedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%.9g,%.9g,%.9g) after "
                             "%u bounces, expected exactly (%.9g,%.9g,%.9g) -- Throughput did not "
                             "survive every dispatch boundary intact\n",
                             i, static_cast<double>(throughputR[i]), static_cast<double>(throughputG[i]),
                             static_cast<double>(throughputB[i]), kBounces,
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedThroughput), static_cast<double>(kAlbedo),
                    kCapacity);

        // 15. Per-bounce RNG parity for one chosen path.
        ohao::diff::PathRng cpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = cpuRng.next1D();
            const float cpuU2 = cpuRng.next1D();
            // Draws 3, 4 and 5 of the bounce: wf_scatter.comp's
            // lobe-selection sample and the environment sample's two
            // uniforms. Their values are not recorded in the debug sink
            // (which holds three floats per path and spends the third on the
            // draw count), but they MUST be consumed here or every later
            // bounce's u1/u2 comparison would be off by that many stream
            // positions.
            (void)cpuRng.next1D();
            (void)cpuRng.next1D();
            (void)cpuRng.next1D();
            const std::uint32_t cpuDrawCount = cpuRng.drawCount();

            const std::vector<float>& gpuDraws = drawsPerBounce[b];
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u RNG diverges at bounce %u: CPU "
                             "(%.9g,%.9g), GPU (%.9g,%.9g) -- wf_scatter.comp's reconstruction "
                             "from (pixelIndex, sampleIndex, bounce) does not match "
                             "ohao::diff::PathRng replayed the same number of draws\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount diverges at bounce %u: CPU "
                             "%u, GPU %u (expected %u -- (bounce+1)*%u)\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount, (b + 1) * kDrawsPerBounce,
                             kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount at bounce %u = %u, expected "
                             "%u\n",
                             kChosenPath, b, cpuDrawCount, (b + 1) * kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u dispatch boundaries for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 16-18. THE FUSED BOUNCE LOOP (ohao::diff::WavefrontLoop, Stage 0b-2a
    // Task 3). Everything above this point runs one wavefront stage per
    // command-buffer submission, with a vkQueueWaitIdle between every stage.
    // That idle wait is a full device barrier: it silently satisfies every
    // ordering requirement the stages have on each other, which is why
    // checks 12-15 can pass with barriers that would be wrong in a real
    // integrator. This block removes it. generate, and then
    // prepare_indirect/intersect/prepare_indirect/scatter once per bounce,
    // are recorded into ONE command buffer, and ordering becomes the
    // barriers' job for the first time (see wavefront_loop.hpp).
    //
    // The assertions are deliberately the SAME two analytic properties
    // checks 14 and 15 already prove stage-by-stage -- throughput exactly
    // albedo^bounces, and per-bounce RNG draws bit-identical to
    // ohao::diff::PathRng -- not new, weaker ones. The whole point is to
    // re-run a property that is already known to hold under the idle wait,
    // against the same shaders, with only the synchronization changed.
    //
    // 16. Every path survives every bounce, and the live ring holds each
    //     path index exactly once -- the compaction offsets are not
    //     displaced. This is the check that catches a stale (unzeroed)
    //     destination counter slot: an atomicAdd based on a non-zero
    //     starting value hands out offsets past the end of the ring, whose
    //     writes wf_scatter.comp's `dstSlot < capacity` guard then drops,
    //     leaving holes.
    // 17. Throughput after 4 fused bounces is exactly 0.0625.
    // 18. Per-bounce RNG draws match ohao::diff::PathRng exactly.
    {
        // height MUST be 8 because every expected value below is
        // calibrated to exactly 512 paths at 64x8 -- the 0.0625 throughput,
        // the per-bounce PathRng parity for kChosenPath, the live counts.
        // It is NOT a dispatch limitation: WavefrontStage::Fixed carries
        // groupsY/groupsZ and can dispatch a genuine 3-D grid, so a stage
        // recorded through WavefrontLoop can cover any resolution just as
        // checks 8-10's hand-recorded 2-D generate dispatch does.
        // runWavefrontFusedLoopProbe leaves groupsY/groupsZ at 1 and
        // dispatches (width/8, 1, 1) x local_size (8,8), covering exactly 8
        // pixel rows. Changing the resolution means recomputing the
        // expectations here first.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        // Must match wf_scatter.comp's kDrawsPerBounce. It became 3 in Stage
        // 0b-2b Task 2 (the BSDF draws a 2-D direction sample AND a 1-D lobe
        // choice every bounce) and 5 in Task 3 (two more for the environment
        // importance sample). The two VALUES compared below are unchanged
        // through both: the shader still draws the direction sample FIRST and
        // appends new draws after the old ones, so the debug sink still
        // records the same two stream positions -- only the per-bounce stride
        // moved, which is precisely what this constant exists to pin. Get it
        // wrong and the fast-forward in the shader and the replay here walk
        // different streams, which is the failure this check exists for.
        constexpr uint32_t kDrawsPerBounce = 5;
        constexpr uint32_t kChosenPath = 333;    // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> fusedDraws;
        std::vector<uint32_t> fusedLiveCounts;
        std::vector<uint32_t> fusedFinalQueue;
        if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                            fusedDraws, fusedLiveCounts, fusedFinalQueue)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // 16. Survivors and compaction integrity.
        //
        // A live count of exactly kCapacity at every bounce depth is not a
        // given: it is what the closed-box scene (see
        // runWavefrontFusedLoopProbe's doc comment) is built to guarantee,
        // and asserting it is what stops the throughput check below from
        // passing vacuously over an empty survivor set.
        if (fusedLiveCounts.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu live counts, expected "
                         "%u\n",
                         fusedLiveCounts.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t b = 0; b < kBounces; ++b) {
            if (fusedLiveCounts[b] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop of %u bounces left %u live paths, "
                             "expected all %u -- survival here is conditional on wf_intersect.comp "
                             "writing the CORRECT stored normal (see the survival derivation's "
                             "\"Normal\" step in gpu_probe_context.cpp), so a wrong or missing "
                             "normal sending a path's scattered direction out through the wrong "
                             "face is the most likely cause; also consider a path that escaped the "
                             "closed box scene some other way, or a compaction counter slot not "
                             "zeroed before its atomicAdd\n",
                             b + 1u, fusedLiveCounts[b], kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // Same sorted-prefix inspection check 12/14 apply to the
        // stage-by-stage rings: the scalar count alone cannot distinguish
        // "every path re-queued exactly once" from "one slot overwritten and
        // another dropped, with the atomicAdd total landing on kCapacity
        // anyway."
        if (fusedFinalQueue.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop final ring readback size %zu, expected "
                         "%u\n",
                         fusedFinalQueue.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sortedFused = fusedFinalQueue;
        std::sort(sortedFused.begin(), sortedFused.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sortedFused[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop final ring sorted[%u] = %u, "
                             "expected %u (duplicate or missing path index -- compaction offsets "
                             "are displaced)\n",
                             i, sortedFused[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop kept all %u paths alive through %u bounces "
                    "and its final ring holds each path index exactly once\n",
                    kCapacity, kBounces);

        // 17. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f for the same reason check 14 hardcodes it:
        // deriving it from kAlbedo would perturb both sides of the
        // comparison identically and the check could never fail.
        // 0.5*0.5*0.5*0.5 = 0.0625, arithmetic done by hand.
        constexpr float expectedFusedThroughput = 0.0625f;

        const std::vector<float> fusedR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> fusedG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> fusedB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (fusedR.size() != kCapacity || fusedG.size() != kCapacity ||
            fusedB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32.
            if (fusedR[i] != expectedFusedThroughput || fusedG[i] != expectedFusedThroughput ||
                fusedB[i] != expectedFusedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u throughput = "
                             "(%.9g,%.9g,%.9g) after %u bounces, expected exactly "
                             "(%.9g,%.9g,%.9g) -- a bounce ran the wrong number of times, or "
                             "Throughput did not survive a dispatch boundary inside the fused "
                             "command buffer\n",
                             i, static_cast<double>(fusedR[i]), static_cast<double>(fusedG[i]),
                             static_cast<double>(fusedB[i]), kBounces,
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedFusedThroughput),
                    static_cast<double>(kAlbedo), kCapacity);

        // 18. Per-bounce RNG parity, identical in form to check 15.
        // fusedDraws[b] is what bounce b's scatter dispatch wrote; see
        // runWavefrontFusedLoopProbe's doc comment for how each bounce's
        // draws are observed despite wf_scatter.comp writing them at a fixed
        // per-path offset.
        if (fusedDraws.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu draw sets, expected %u\n",
                         fusedDraws.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        ohao::diff::PathRng fusedCpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = fusedCpuRng.next1D();
            const float cpuU2 = fusedCpuRng.next1D();
            // wf_scatter.comp's lobe-selection sample and the environment
            // sample's two uniforms -- see check 15.
            (void)fusedCpuRng.next1D();
            (void)fusedCpuRng.next1D();
            (void)fusedCpuRng.next1D();
            const std::uint32_t cpuDrawCount = fusedCpuRng.drawCount();

            const std::vector<float>& gpuDraws = fusedDraws[b];
            if (gpuDraws.size() != static_cast<std::size_t>(kCapacity) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop bounce %u debug-draws size %zu, "
                             "expected %zu\n",
                             b, gpuDraws.size(), static_cast<std::size_t>(kCapacity) * 3u);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u RNG diverges at bounce %u: "
                             "CPU (%.9g,%.9g), GPU (%.9g,%.9g) -- the fused loop replayed a "
                             "different random stream than ohao::diff::PathRng\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount || cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u drawCount at bounce %u: "
                             "CPU %u, GPU %u, expected %u ((bounce+1)*%u) -- the Bounce field the "
                             "fast-forward reads did not advance exactly once per fused bounce\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount,
                             (b + 1) * kDrawsPerBounce, kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u fused bounces for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 19. GEOMETRIC NORMALS (Stage 0b-2b Task 1). wf_intersect.comp must
    // write the hit's real, forward-facing geometric normal into path state
    // (PathStateField::NormalX/Y/Z), and this asserts it against an oracle
    // that is pure analytic geometry computed here on the host -- the box's
    // face planes and the camera's closed-form ray -- with nothing the
    // shader computed anywhere in it. (The Dir field the GPU wrote is
    // deliberately NOT read: it is check 8's subject, not this check's
    // oracle.)
    //
    // WHY A BOX AND NOT THE QUAD every other intersect check uses: the
    // single quad at z = -planeDistance, seen from a camera at the origin
    // looking down -Z, has exactly one forward-facing normal, (0,0,1) --
    // which is precisely the value wf_scatter.comp used to hardcode. A
    // check built on that scene cannot tell a real normal from the constant.
    // A closed box entered from its centre reaches five of its six faces, so
    // five distinct analytic normals are asserted, and the box is wound
    // OUTWARD (see buildAxisAlignedBoxGeometry) so that every one of those
    // hits also has to go through the flip-to-oppose-the-ray step.
    //
    // GEOMETRY OF THE ORACLE. The camera sits at the box centre, so for a
    // unit direction d the exit distance through the face on axis k is
    // t_k = E / |d_k|; the face actually hit is the argmin over k, i.e. the
    // argmax of |d_k| (E is the same on all three axes). Its inward normal
    // -- which is the forward-facing one, since the ray leaves the box
    // through that face -- is -sign(d_k) * e_k.
    //
    // TIE-FREEDOM IS BY CONSTRUCTION, NOT BY LUCK. That argmax is only
    // well-defined if no two |d_k| are equal. With kW even and kH ODD:
    //   |d_x| ~ |2x + 1 - kW| * tanHalfFov / kH  (odd numerator)
    //   |d_y| ~ |kH - 2y - 1| * tanHalfFov / kH  (even numerator)
    // so |d_x| == |d_y| would need an odd integer to equal an even one, and
    // |d_x| == |d_z| (== 1 before normalisation) would need
    // |2x + 1 - kW| == kH / tanHalfFov == 24.5, not an integer. The closest
    // approach of any pair is therefore >= 0.5 * tanHalfFov / kH in
    // pre-normalisation units, and the loop below asserts a hard margin on
    // the normalised directions anyway, so a future change to kW/kH/FOV that
    // reintroduced a tie fails loudly here instead of silently comparing
    // against whichever face the GPU happened to pick.
    //
    // TOLERANCES. The two off-axis components are required to be BIT-EXACTLY
    // zero: the box's edge vectors are exactly axis-aligned, so
    // cross(v1 - v0, v2 - v0) is exactly zero on those two axes, and
    // multiplying an exact zero by any finite normalisation factor (or
    // negating it) stays zero. Only the remaining component passes through
    // normalize(), whose inversesqrt() GLSL permits to be up to 2 ULP off
    // (~2.4e-7 relative), so it is compared to +/-1 with a 1e-6 bound --
    // roughly 4x that spec limit, and six orders of magnitude tighter than
    // the distance to any other face's normal. HitT gets a relative bound of
    // 1e-4, loose enough for the ray-triangle solve and far tighter than the
    // gap between adjacent faces' distances.
    //
    // LIMITATION: this check CANNOT distinguish a face from its opposite.
    // The stored normal's sign comes entirely from wf_intersect.comp's flip
    // against the ray direction, not from which primitive was actually hit:
    // for any triangle whose cross product lies on axis k, the stored
    // normal is -sign(d_k) * e_k regardless of which triangle's index was
    // actually looked up. buildAxisAlignedBoxGeometry emits each face's two
    // triangles adjacently, axis by axis (tris 0,1 = +X; 2,3 = -X; 4,5 =
    // +Y; 6,7 = -Y; 8,9 = +Z; 10,11 = -Z), so a primitive-index bug of the
    // form `primitive ^ 1` (the OTHER triangle of the SAME face) or
    // `primitive ^ 2` (the OPPOSITE face, same axis) reads back a triangle
    // whose cross product still lies on the same axis with the same sign,
    // so it still normalize()s to the exact expected normal -- and HitT
    // (from rayQueryGetIntersectionTEXT, which never goes through the index
    // lookup at all) is unaffected by the bug in the first place. Both
    // assertions above would pass with the wrong triangle read. The
    // residual bug class this leaves uncaught is narrow -- an off-by-a-
    // different-amount indexing bug such as `primitive + 1` still fails
    // outright on 3 of the 6 faces, and a stride or scale error in the
    // index lookup fails everywhere -- but it is real, and this check does
    // not close it. It is not weakened to pretend otherwise; this is simply
    // what it does not cover.
    {
        // kH is ODD and kW EVEN on purpose -- see the tie-freedom note above.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 49;
        constexpr uint32_t kCapacity = kW * kH;  // 3136
        static_assert(kW % 2 == 0 && kH % 2 == 1,
                      "the argmax oracle's tie-freedom argument needs kW even and kH odd");
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
        // Wide on purpose: at the default 0.2 every ray would hit the -Z
        // face and the check would degenerate to the one normal the old
        // hardcoded constant already had.
        constexpr float kTanHalfFov = 2.0f;
        constexpr float kBoxHalfExtent = 4.0f;
        constexpr float kNormalTolerance = 1e-6f;
        constexpr float kHitTRelTolerance = 1e-4f;
        // Minimum separation required between the largest and second-largest
        // |d_k| for the argmin face to be unambiguous. The construction above
        // guarantees >= 0.5 * kTanHalfFov / kH / |d| ~ 0.006; this is an
        // order of magnitude below that and ~4 orders above float noise.
        constexpr float kFaceMargin = 1e-3f;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Camera at the box CENTRE, so every ray hits a face and the exit
        // distance is E / |d_k| with no origin offset term.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        std::vector<uint32_t> boxQueue1;
        if (!ctx.runWavefrontBoxIntersectProbe(wf, kBoxHalfExtent, boxQueue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // A ray from strictly inside a closed convex body always leaves it
        // through a face, so nothing may miss. If this trips, the readbacks
        // below would be comparing against normals for hits that never
        // happened.
        const std::uint32_t boxSurvivors =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (boxSurvivors != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: normal probe: %u of %u paths hit the closed box, "
                         "expected all of them (a ray from the interior of a convex body cannot "
                         "miss it)\n",
                         boxSurvivors, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> nx =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalX);
        const std::vector<float> ny =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalY);
        const std::vector<float> nz =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalZ);
        const std::vector<float> hitT =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (nx.size() != kCapacity || ny.size() != kCapacity || nz.size() != kCapacity ||
            hitT.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe field readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // faceHits[2*k + (sign < 0)] -- six buckets, one per box face.
        uint32_t faceHits[6] = {0, 0, 0, 0, 0, 0};
        float maxNormalError = 0.0f;
        float maxHitTRelError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;

                // --- Analytic ray, host-side. Identical construction to
                // check 8's (and to camera_ray.glsl's), recomputed here
                // rather than read out of the Dir field so that nothing the
                // shader produced enters the oracle. ---
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                float d[3] = {ndcX * kAspect * kTanHalfFov, ndcY * kTanHalfFov, -1.0f};
                const float invLen =
                    1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                d[0] *= invLen;
                d[1] *= invLen;
                d[2] *= invLen;

                // --- Analytic face: argmax |d_k|, with the tie margin
                // enforced rather than assumed. ---
                uint32_t axis = 0;
                for (uint32_t k = 1; k < 3u; ++k) {
                    if (std::fabs(d[k]) > std::fabs(d[axis])) axis = k;
                }
                float secondAbs = 0.0f;
                for (uint32_t k = 0; k < 3u; ++k) {
                    if (k != axis) secondAbs = std::max(secondAbs, std::fabs(d[k]));
                }
                if (std::fabs(d[axis]) - secondAbs < kFaceMargin) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u): the box face "
                                 "this ray exits through is ambiguous -- |d| = (%.9g,%.9g,%.9g), "
                                 "largest exceeds runner-up by only %.9g < %.9g. The oracle's "
                                 "argmax is not well defined, so kW/kH/tanHalfFov must be chosen "
                                 "to keep every pair of |d_k| apart (see this check's "
                                 "tie-freedom note)\n",
                                 x, y, static_cast<double>(std::fabs(d[0])),
                                 static_cast<double>(std::fabs(d[1])),
                                 static_cast<double>(std::fabs(d[2])),
                                 static_cast<double>(std::fabs(d[axis]) - secondAbs),
                                 static_cast<double>(kFaceMargin));
                    wf.destroy(ctx.allocator());
                    return 1;
                }

                // Inward (== forward-facing) normal of the exit face, and
                // the exit distance, both straight from the box's algebra.
                float expected[3] = {0.0f, 0.0f, 0.0f};
                expected[axis] = (d[axis] > 0.0f) ? -1.0f : 1.0f;
                const float expectedT = kBoxHalfExtent / std::fabs(d[axis]);
                faceHits[2u * axis + ((d[axis] > 0.0f) ? 0u : 1u)] += 1u;

                const float actual[3] = {nx[i], ny[i], nz[i]};
                for (uint32_t k = 0; k < 3u; ++k) {
                    const float err = std::fabs(actual[k] - expected[k]);
                    maxNormalError = std::max(maxNormalError, err);
                    // Off-axis components: bit-exact zero (see this check's
                    // tolerance note). On-axis: within kNormalTolerance.
                    const bool bad = (k == axis) ? (err > kNormalTolerance)
                                                 : (actual[k] != 0.0f);
                    if (bad) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) path %u: "
                                     "stored normal = (%.9g,%.9g,%.9g), analytic box-face normal "
                                     "= (%.9g,%.9g,%.9g) (face: axis %u at %+.1f). Component %u "
                                     "differs by %.9g -- wf_intersect.comp is not writing the "
                                     "real geometric normal of the hit\n",
                                     x, y, i, static_cast<double>(actual[0]),
                                     static_cast<double>(actual[1]),
                                     static_cast<double>(actual[2]),
                                     static_cast<double>(expected[0]),
                                     static_cast<double>(expected[1]),
                                     static_cast<double>(expected[2]), axis,
                                     static_cast<double>(-expected[axis] * kBoxHalfExtent), k,
                                     static_cast<double>(err));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }

                const float relT = std::fabs(hitT[i] - expectedT) / expectedT;
                maxHitTRelError = std::max(maxHitTRelError, relT);
                if (relT > kHitTRelTolerance) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) hit distance "
                                 "%.9g, analytic box exit distance %.9g (rel err %.9g) -- the hit "
                                 "is not on the face the oracle's normal belongs to\n",
                                 x, y, static_cast<double>(hitT[i]),
                                 static_cast<double>(expectedT), static_cast<double>(relT));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        // Non-degeneracy: this scene reaches exactly FIVE of the box's six
        // faces, not "at least three" -- the sixth, +Z, is unreachable BY
        // CONSTRUCTION, not by bad luck on this run. The camera used above
        // is WavefrontGenerateCamera's default (only tanHalfFov is
        // overridden), whose forward is (0,0,-1); camera_ray.glsl's
        // dir = normalize(forward + right*(...) + up*(...)) only ever picks
        // up a z-component from `forward` (right and up are both z == 0
        // here), so every primary ray has d_z < 0 and none can exit through
        // +Z. faceHits[4] is +Z's bucket (2*axis + (d[axis]>0 ? 0 : 1) with
        // axis == 2, sign > 0 -- see the faceHits indexing comment above the
        // loop), so it must be exactly 0; the other five buckets (+X -X +Y
        // -Y -Z: faceHits[0,1,2,3,5]) must each be nonzero. Asserting each
        // face individually, rather than a floor on the count reached, is
        // what makes a future FOV or resolution change that collapsed
        // coverage to three faces fail here instead of passing silently --
        // and it also catches the oracle's own ray model being wrong: if
        // +Z ever came out nonzero, a sign got flipped somewhere and this
        // check's normals could no longer be trusted either.
        //
        // (-Z alone always totals exactly 600 at this kW/kH/tanHalfFov: the
        // argmax-of-|d_k| condition works out to |2x+1-kW| < kH/tanHalfFov,
        // i.e. |2x+1-64| < 24.5, giving x in [20,43] (24 columns), and
        // |kH-2y-1| < kH/tanHalfFov, i.e. |48-2y| < 24.5, giving y in
        // [12,36] (25 rows); 24*25 == 600. Not asserted as an exact count
        // here -- that arithmetic belongs in a comment, not baked into a
        // brittle assertion -- but it is why -Z's count in the OK: line
        // below is always 600.)
        static constexpr uint32_t kUnreachableFaceZPlus = 4u;
        static constexpr const char* kFaceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        bool faceCoverageOk = true;
        for (uint32_t f = 0; f < 6u; ++f) {
            const bool shouldBeReached = (f != kUnreachableFaceZPlus);
            const bool reached = faceHits[f] != 0u;
            if (reached != shouldBeReached) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: normal probe face %s (bucket %u) got %u hits, "
                             "expected %s -- +Z is unreachable by construction (camera forward is "
                             "(0,0,-1), so d_z < 0 for every primary ray) and the other five faces "
                             "must each be reached at least once (+X %u, -X %u, +Y %u, -Y %u, "
                             "+Z %u, -Z %u)\n",
                             kFaceNames[f], f, faceHits[f], shouldBeReached ? "nonzero" : "exactly 0",
                             faceHits[0], faceHits[1], faceHits[2], faceHits[3], faceHits[4],
                             faceHits[5]);
                faceCoverageOk = false;
            }
        }
        if (!faceCoverageOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        uint32_t facesReached = 0;
        for (uint32_t f = 0; f < 6u; ++f) {
            if (faceHits[f] != 0u) ++facesReached;
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect geometric normals match the analytic "
                    "box-face normals for all %u paths across %u distinct faces "
                    "(+X %u, -X %u, +Y %u, -Y %u, +Z %u, -Z %u; max |normal err| = %g, "
                    "max relative HitT err = %g)\n",
                    kCapacity, facesReached, faceHits[0], faceHits[1], faceHits[2], faceHits[3],
                    faceHits[4], faceHits[5], static_cast<double>(maxNormalError),
                    static_cast<double>(maxHitTRelError));

        wf.destroy(ctx.allocator());
    }

    // 20. THE BSDF ITSELF (Stage 0b-2b Task 2), term by term, against the
    // independent CPU oracle at the top of this file -- whose formulas come
    // from Walter et al. 2007, Heitz 2014, Heitz 2018, Schlick 1994 and
    // PBRT, NOT from the GLSL under test. See that section's header for what
    // is paper-derived (all of f; the physics inside pdf) and what is
    // instead the documented sampling contract (the lobe probability), and
    // for why the distinction matters to how much this check can prove.
    //
    // Three separate assertions per case, all against the oracle:
    //   (a) f(N,V,L)   -- the BSDF value at a host-chosen L.
    //   (b) pdf(N,V,L) -- the sampling density at that same L.
    //   (c) the SAMPLER's returned weight equals oracle_f(L') * (N.L') /
    //       oracle_pdf(L') at the direction L' the GPU actually sampled.
    // (c) is what ties the sampler to the evaluator: a sampler that draws
    // from one distribution and divides by another's density passes (a) and
    // (b) and fails here. The oracle recomputes f and pdf at L' itself -- it
    // never reuses the GPU's own f/pdf outputs -- so (c) cannot agree by
    // construction either.
    {
        constexpr uint32_t kFloatsPerCase = 12;  // must match bsdf_probe.comp's output layout

        struct MaterialSpec {
            const char* name;
            double roughness;
            double metallic;
            double specularWeight;
            double baseColor[3];
        };
        // Deliberately spans: the pure-Lambert configuration the wavefront
        // probes run with (so this check covers the exact material checks 14
        // and 17 depend on), a glossy dielectric, a sharp conductor, a rough
        // conductor, and a half-weight dielectric where the lobe mixture is
        // genuinely a mixture rather than degenerate at one end.
        const MaterialSpec kMaterials[] = {
            {"lambert", 1.00, 0.0, 0.0, {0.5, 0.5, 0.5}},
            {"dielectric-glossy", 0.35, 0.0, 1.0, {0.8, 0.6, 0.2}},
            {"conductor-sharp", 0.15, 1.0, 1.0, {0.9, 0.85, 0.5}},
            {"conductor-rough", 0.80, 1.0, 1.0, {0.3, 0.4, 0.9}},
            {"dielectric-half", 0.50, 0.0, 0.5, {0.2, 0.7, 0.3}},
        };
        // Two normals: the axis-aligned one every earlier probe sees, and a
        // tilted one, so a BSDF that silently assumed N = +Z (as the Stage
        // 0b-1 scatter placeholder effectively did) cannot pass.
        const OracleVec3 kNormals[] = {{0.0, 0.0, 1.0},
                                       oracleNormalize(OracleVec3{0.3, -0.5, 0.81})};
        const double kViewThetas[] = {0.20, 0.70, 1.20};
        const double kLightAngles[][2] = {{0.30, 0.0}, {0.90, 2.0}, {1.30, 4.5}};
        // The view's AZIMUTH about N, cycled independently of everything
        // else. An isotropic BSDF must be invariant to it, and until this was
        // a list the whole table shared one value (0.6) -- so nothing tested
        // that invariance, and a tangent-frame-dependent bug that only
        // showed up at some azimuths could not be seen. 5 is coprime with the
        // 7 sample triples below, so the pairing does not lock into a short
        // cycle over the 90 cases.
        const double kViewPhis[] = {0.6, 1.9, 3.3, 4.7, 5.9};
        // Sample values cycled through the cases so both lobes get chosen
        // somewhere in the table (uLobe below/above the specular probability)
        // and the VNDF sampler is exercised across its unit square. Seven
        // triples, not three: with three, 90 cases carried only three
        // distinct (u1, u2, uLobe) points, so the sampler was being asked the
        // same question thirty times over. The first three are the original
        // ones, kept so the coverage this table already had is not traded
        // away for the new coverage.
        const float kSamples[][3] = {{0.13f, 0.77f, 0.05f},
                                     {0.61f, 0.24f, 0.45f},
                                     {0.89f, 0.52f, 0.95f},
                                     {0.03f, 0.41f, 0.28f},
                                     {0.37f, 0.95f, 0.68f},
                                     {0.72f, 0.09f, 0.11f},
                                     {0.96f, 0.63f, 0.82f}};
        constexpr uint32_t kSampleCount = sizeof(kSamples) / sizeof(kSamples[0]);
        constexpr uint32_t kViewPhiCount = sizeof(kViewPhis) / sizeof(kViewPhis[0]);

        constexpr uint32_t kMaterialCount = sizeof(kMaterials) / sizeof(kMaterials[0]);
        constexpr uint32_t kNormalCount = 2;
        constexpr uint32_t kViewCount = 3;
        constexpr uint32_t kLightCount = 3;
        constexpr uint32_t kCaseCount = kMaterialCount * kNormalCount * kViewCount * kLightCount;

        ohao::diff::ArenaLayout bsdfLayout;
        const std::size_t bsdfBlock = bsdfLayout.add(kCaseCount * kFloatsPerCase);
        ohao::diff::GradientArena bsdfArena;
        if (!bsdfArena.build(ctx.allocator(), bsdfLayout)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: bsdf probe arena build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { bsdfArena.zero(cmd); });

        // Host-side record of each case, so the oracle can be evaluated
        // after every dispatch has landed.
        struct CaseRecord {
            const char* materialName;
            OracleVec3 N;
            OracleVec3 V;
            OracleVec3 L;
            OracleMaterial material;
            double u1;
            double u2;
            double uLobe;
        };
        std::vector<CaseRecord> records;
        records.reserve(kCaseCount);

        bool bsdfDispatchOk = true;
        uint32_t caseIndex = 0;
        for (uint32_t mi = 0; mi < kMaterialCount && bsdfDispatchOk; ++mi) {
            for (uint32_t ni = 0; ni < kNormalCount && bsdfDispatchOk; ++ni) {
                for (uint32_t vi = 0; vi < kViewCount && bsdfDispatchOk; ++vi) {
                    for (uint32_t li = 0; li < kLightCount && bsdfDispatchOk; ++li) {
                        const MaterialSpec& ms = kMaterials[mi];
                        const OracleVec3 N = kNormals[ni];
                        const double viewPhi = kViewPhis[caseIndex % kViewPhiCount];
                        const OracleVec3 V = oracleDirFromAngles(N, kViewThetas[vi], viewPhi);
                        const OracleVec3 L =
                            oracleDirFromAngles(N, kLightAngles[li][0], kLightAngles[li][1]);

                        OracleMaterial mat;
                        mat.baseColor = {ms.baseColor[0], ms.baseColor[1], ms.baseColor[2]};
                        mat.roughness = ms.roughness;
                        mat.metallic = ms.metallic;
                        mat.specularWeight = ms.specularWeight;

                        const float* smp = kSamples[caseIndex % kSampleCount];

                        ohao::diff::BsdfProbeCase probeCase;
                        probeCase.normal[0] = static_cast<float>(N.x);
                        probeCase.normal[1] = static_cast<float>(N.y);
                        probeCase.normal[2] = static_cast<float>(N.z);
                        probeCase.roughness = static_cast<float>(ms.roughness);
                        probeCase.view[0] = static_cast<float>(V.x);
                        probeCase.view[1] = static_cast<float>(V.y);
                        probeCase.view[2] = static_cast<float>(V.z);
                        probeCase.metallic = static_cast<float>(ms.metallic);
                        probeCase.light[0] = static_cast<float>(L.x);
                        probeCase.light[1] = static_cast<float>(L.y);
                        probeCase.light[2] = static_cast<float>(L.z);
                        probeCase.specularWeight = static_cast<float>(ms.specularWeight);
                        probeCase.baseColor[0] = static_cast<float>(ms.baseColor[0]);
                        probeCase.baseColor[1] = static_cast<float>(ms.baseColor[1]);
                        probeCase.baseColor[2] = static_cast<float>(ms.baseColor[2]);
                        probeCase.u1 = smp[0];
                        probeCase.u2 = smp[1];
                        probeCase.uLobe = smp[2];
                        probeCase.outIndex = caseIndex;

                        if (!ctx.runBsdfProbe(bsdfArena, probeCase)) {
                            std::fprintf(stderr,
                                         "[diff_gpu_probe] FAIL: bsdf probe dispatch for case %u\n",
                                         caseIndex);
                            bsdfDispatchOk = false;
                            break;
                        }
                        records.push_back(CaseRecord{ms.name, N, V, L, mat, smp[0], smp[1],
                                                     smp[2]});
                        ++caseIndex;
                    }
                }
            }
        }
        if (!bsdfDispatchOk) {
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> bsdfOut = bsdfArena.readback(ctx.allocator(), bsdfBlock);
        if (bsdfOut.size() < static_cast<std::size_t>(kCaseCount) * kFloatsPerCase) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: bsdf probe readback returned %zu floats, expected "
                         "at least %u\n",
                         bsdfOut.size(), kCaseCount * kFloatsPerCase);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        // TOLERANCES, and why they are not one number. The GPU works in
        // float32 and the oracle in double, so some slack is unavoidable;
        // how much depends on the CONDITIONING of the quantity, which is
        // very different for the value and for the density.
        //
        //   f and the sampler weight tolerate 1e-4. Both are smooth in their
        //   inputs at every case in the table, and the measured worst case
        //   over the whole table is ~6e-6 -- a factor of 16 of headroom.
        //
        //   pdf tolerates 1e-3. At the sharpest material here (roughness
        //   0.15, alpha^2 ~ 5e-4) the GGX lobe's angular width is comparable
        //   to alpha, so d(ln D)/d(N.H) ~ 4/alpha^2 ~ 8e3: a float32
        //   direction carrying ~1e-7 of relative error comes back with
        //   ~8e-4 of relative error in D, and therefore in the density.
        //   That is arithmetic conditioning, not a modelling difference, and
        //   the measured worst case (~3.9e-4) sits where the estimate
        //   predicts.
        //
        // Neither number was tuned until it passed. Both are printed with
        // the observed maxima on the OK: line, so a tolerance that has
        // quietly started to absorb a real error is visible rather than
        // silent.
        //
        // For scale, the smallest modelling error this is meant to catch --
        // swapping Schlick's exponent 5 for 4 -- was measured by making that
        // edit: the run aborts at case 20 (dielectric-glossy) with a
        // deviation of 4.8e-4, i.e. 4.8x the f tolerance and ~140x the
        // observed float32 noise floor of ~3.4e-6. It is NOT true that every
        // case moves by that much: cases 18 and 19 are the same material and
        // moved by LESS than the tolerance, so they did not register at all.
        // What catches this class of error is the BREADTH of the table -- the
        // spread of view, light and normal geometry means some case is
        // sensitive -- not the sensitivity of any individual case. Shrinking
        // the table would weaken the check even with the tolerance untouched.
        constexpr double kBsdfValueRelTol = 1e-4;
        constexpr double kBsdfPdfRelTol = 1e-3;

        double maxFErr = 0.0;
        double maxPdfErr = 0.0;
        double maxWeightErr = 0.0;
        uint32_t specularSampledCount = 0;
        uint32_t diffuseSampledCount = 0;
        uint32_t rejectedSampleCount = 0;
        uint32_t grazingRejectionCount = 0;
        uint32_t branchAssertedCount = 0;
        // How close uLobe may sit to q before the branch-agreement assertion
        // below stands down. At |uLobe - q| under this the GPU's float32 q
        // and the oracle's double q can legitimately land on opposite sides
        // of the comparison, and asserting there would be asserting float
        // rounding. The count of cases actually asserted is printed, so a
        // margin that quietly disabled the assertion for the whole table
        // would be visible.
        constexpr double kLobeDecisionMargin = 1e-3;

        for (uint32_t i = 0; i < kCaseCount; ++i) {
            const CaseRecord& rec = records[i];
            const float* out = &bsdfOut[static_cast<std::size_t>(i) * kFloatsPerCase];

            OracleVec3 refF;
            double refPdf = 0.0;
            oracleBsdfEval(rec.N, rec.V, rec.L, rec.material, refF, refPdf);

            const double gpuF[3] = {out[0], out[1], out[2]};
            const double refFv[3] = {refF.x, refF.y, refF.z};
            for (int c = 0; c < 3; ++c) {
                const double e = oracleRelDiff(refFv[c], gpuF[c]);
                if (e > maxFErr) maxFErr = e;
                if (e > kBsdfValueRelTol) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF f mismatch, case %u (%s), channel "
                                 "%d: GPU %.9g, CPU oracle %.9g (relative %.3g > %.3g). N = "
                                 "(%.6f,%.6f,%.6f), V = (%.6f,%.6f,%.6f), L = (%.6f,%.6f,%.6f), "
                                 "roughness %.3f, metallic %.3f, specularWeight %.3f\n",
                                 i, rec.materialName, c, gpuF[c], refFv[c], e, kBsdfValueRelTol,
                                 rec.N.x, rec.N.y, rec.N.z, rec.V.x, rec.V.y, rec.V.z, rec.L.x,
                                 rec.L.y, rec.L.z, rec.material.roughness, rec.material.metallic,
                                 rec.material.specularWeight);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }

            const double gpuPdf = out[3];
            const double pdfErr = oracleRelDiff(refPdf, gpuPdf);
            if (pdfErr > maxPdfErr) maxPdfErr = pdfErr;
            if (pdfErr > kBsdfPdfRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF pdf mismatch, case %u (%s): GPU %.9g, "
                             "CPU oracle %.9g (relative %.3g > %.3g). roughness %.3f, metallic "
                             "%.3f, specularWeight %.3f\n",
                             i, rec.materialName, gpuPdf, refPdf, pdfErr, kBsdfPdfRelTol,
                             rec.material.roughness, rec.material.metallic,
                             rec.material.specularWeight);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            // (c) The sampler's own weight, at the direction the GPU drew.
            // Slots 4..6 hold that direction EXACTLY as drawn -- including
            // the GGX VNDF's below-horizon tail -- because diffBsdfSample
            // deliberately does not substitute a usable direction for a
            // rejected sample. That is what lets the oracle confirm a zero
            // weight was legitimate instead of having to take it on trust.
            const OracleVec3 sampledL = oracleNormalize({out[4], out[5], out[6]});
            const double gpuWeight[3] = {out[7], out[8], out[9]};
            const double gpuSampPdf = out[10];
            OracleVec3 sampF;
            double sampPdf = 0.0;
            oracleBsdfEval(rec.N, rec.V, sampledL, rec.material, sampF, sampPdf);
            const double sampNdotL = oracleDot(rec.N, sampledL);

            // WHICH LOBE THE GPU ACTUALLY SAMPLED, decided from the returned
            // direction rather than predicted from the material table. The
            // diffuse branch draws diffCosineHemisphere(N, uDir); the
            // specular branch draws a VNDF half-vector and reflects. So if
            // the returned direction IS the cosine-hemisphere direction for
            // this case's uDir, the diffuse branch ran. (The two agreeing by
            // accident is a measure-zero coincidence, and the tolerance here
            // is float32 noise, not a window.) This previously classified by
            // oracleSpecProb(...) > 0.5, which reads only the hardcoded
            // material table and the hardcoded normals: no GPU output entered
            // it, so it was deterministic and could not fail while its FAIL
            // message claimed to describe which branch the sampler took.
            const OracleVec3 cosineL = oracleCosineHemisphere(rec.N, rec.u1, rec.u2);
            const bool tookDiffuseBranch = oracleDistance(sampledL, cosineL) < 1e-4;
            if (tookDiffuseBranch) {
                ++diffuseSampledCount;
            } else {
                ++specularSampledCount;
            }

            // And the branch it took must be the branch the documented
            // strategy asks for: uLobe < q, with q the lobe probability this
            // oracle computes independently. This is the only place the
            // lobe-selection rule itself is asserted per-case -- see the
            // oracle header's note on where q is guarded.
            const double caseQ = oracleSpecProb(rec.material, oracleDot(rec.N, rec.V));
            const bool expectSpecularBranch =
                (rec.uLobe < caseQ) && (oracleDot(rec.N, rec.V) > kShaderGrazingCos);
            if (std::abs(rec.uLobe - caseQ) > kLobeDecisionMargin) {
                ++branchAssertedCount;
                if (expectSpecularBranch == tookDiffuseBranch) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) took the %s "
                                 "branch, but the contract's lobe probability q = %.9g with "
                                 "uLobe = %.9g asks for the %s branch. The branch actually taken "
                                 "was read off the returned direction L = (%.6f,%.6f,%.6f)\n",
                                 i, rec.materialName, tookDiffuseBranch ? "diffuse" : "specular",
                                 caseQ, rec.uLobe, expectSpecularBranch ? "specular" : "diffuse",
                                 sampledL.x, sampledL.y, sampledL.z);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }

            if (gpuSampPdf <= 0.0) {
                // The GPU says it rejected this sample. Two things have to
                // hold, or the rejection is a bug rather than a tail: the
                // ORACLE must independently agree the direction carries no
                // energy, and the weight must be exactly zero. A sampler
                // that rejected everything would fail the first of those on
                // its very first accepted-looking case.
                //
                // One documented exception, and it is a real threshold
                // difference rather than slack: bsdf.glsl refuses the
                // specular math at N.L <= DIFF_BSDF_MIN_COS (1e-4) while this
                // oracle refuses it at N.L <= 0, because 1e-4 is an
                // implementation guard against dividing by (N.L) twice and
                // has no place in the physics. A sample landing in the band
                // (0, 1e-4] is therefore rejected by the shader and accepted
                // by the oracle, legitimately. Such cases are counted and
                // reported rather than being allowed to fail the run; every
                // case in the current table sits orders of magnitude above
                // the band, so the count is expected to be 0 and a nonzero
                // one is worth looking at.
                if (sampNdotL > 0.0 && sampNdotL <= kShaderGrazingCos) {
                    ++grazingRejectionCount;
                } else if (sampNdotL > 0.0 && sampPdf > 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) rejected its "
                                 "own sample (pdf 0) at L = (%.6f,%.6f,%.6f), but the oracle says "
                                 "that direction is perfectly valid (N.L = %.9g, pdf = %.9g) and "
                                 "is not inside the shader's documented grazing band "
                                 "(0, %.0e]\n",
                                 i, rec.materialName, sampledL.x, sampledL.y, sampledL.z,
                                 sampNdotL, sampPdf, kShaderGrazingCos);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
                for (int c = 0; c < 3; ++c) {
                    if (gpuWeight[c] != 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported a "
                                     "zero density but a non-zero weight %.9g -- a zero-BRDF "
                                     "sample must carry a zero weight\n",
                                     i, rec.materialName, gpuWeight[c]);
                        bsdfArena.destroy(ctx.allocator());
                        return 1;
                    }
                }
                ++rejectedSampleCount;
                continue;
            }

            if (sampPdf <= 0.0 || sampNdotL <= 0.0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) accepted a sample "
                             "(pdf %.9g) at L = (%.6f,%.6f,%.6f) that the oracle says carries no "
                             "energy (N.L = %.9g)\n",
                             i, rec.materialName, gpuSampPdf, sampledL.x, sampledL.y, sampledL.z,
                             sampNdotL);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            // The density the sampler says it drew with must be the density
            // the evaluator assigns to that direction. This is what a
            // "samples one distribution, divides by another" bug shows up
            // as, and it is checked against the ORACLE's density, not
            // against the GPU's own eval output.
            const double sampPdfErr = oracleRelDiff(sampPdf, gpuSampPdf);
            if (sampPdfErr > maxPdfErr) maxPdfErr = sampPdfErr;
            if (sampPdfErr > kBsdfPdfRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported density "
                             "%.9g at its own sampled direction, CPU oracle %.9g (relative %.3g "
                             "> %.3g)\n",
                             i, rec.materialName, gpuSampPdf, sampPdf, sampPdfErr,
                             kBsdfPdfRelTol);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            const double refWeight[3] = {sampF.x * sampNdotL / sampPdf,
                                         sampF.y * sampNdotL / sampPdf,
                                         sampF.z * sampNdotL / sampPdf};
            for (int c = 0; c < 3; ++c) {
                const double e = oracleRelDiff(refWeight[c], gpuWeight[c]);
                if (e > maxWeightErr) maxWeightErr = e;
                if (e > kBsdfValueRelTol) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler weight mismatch, case %u "
                                 "(%s), channel %d: GPU %.9g, CPU oracle f*cos/pdf %.9g "
                                 "(relative %.3g > %.3g). Sampled L = (%.6f,%.6f,%.6f), "
                                 "oracle pdf there %.9g -- the sampler and the evaluator "
                                 "disagree about which density the direction was drawn from\n",
                                 i, rec.materialName, c, gpuWeight[c], refWeight[c], e,
                                 kBsdfValueRelTol, sampledL.x, sampledL.y, sampledL.z, sampPdf);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        if (specularSampledCount == 0 || diffuseSampledCount == 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: over %u cases the sampler took only one of its "
                         "two branches (%u specular, %u diffuse, counted from the direction the "
                         "GPU returned) -- the table no longer covers the mixture it claims to\n",
                         kCaseCount, specularSampledCount, diffuseSampledCount);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }
        if (branchAssertedCount * 4u < kCaseCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: the lobe-branch agreement assertion stood down "
                         "on all but %u of %u cases (uLobe within %.0e of q) -- it is no longer "
                         "asserting the lobe-selection rule over a meaningful part of the "
                         "table\n",
                         branchAssertedCount, kCaseCount, kLobeDecisionMargin);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        // Non-vacuity guard on assertion (c): every rejected sample skips the
        // weight comparison, so a sampler that rejected most of the table
        // could pass (c) having compared almost nothing. The rejected cases
        // are still individually verified against the oracle above -- this
        // bounds how much of the table that verification is allowed to be.
        if (rejectedSampleCount * 4u >= kCaseCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: BSDF sampler rejected %u of %u cases -- the "
                         "weight comparison is no longer exercising most of the table\n",
                         rejectedSampleCount, kCaseCount);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        std::printf("[diff_gpu_probe] OK: BSDF f, pdf and sampler weight match an independent "
                    "CPU oracle (Walter 2007 / Heitz 2014 / Heitz 2018 / Schlick 1994) over %u "
                    "cases x %u materials x %u sample triples x %u view azimuths; the GPU took "
                    "the specular branch %u times and the diffuse branch %u times (measured from "
                    "the returned direction), and the branch matched the contract's uLobe < q on "
                    "all %u cases where the assertion applied; %u below-horizon rejections, %u "
                    "inside the shader's grazing band; max relative error f %.3g, weight %.3g "
                    "(tolerance %.3g), pdf %.3g (tolerance %.3g)\n",
                    kCaseCount, kMaterialCount, kSampleCount, kViewPhiCount,
                    specularSampledCount, diffuseSampledCount, branchAssertedCount,
                    rejectedSampleCount, grazingRejectionCount, maxFErr, maxWeightErr,
                    kBsdfValueRelTol, maxPdfErr, kBsdfPdfRelTol);

        bsdfArena.destroy(ctx.allocator());
    }

    // 21-23. THE FURNACE TEST, on its own deliberately trivial scene.
    //
    // Check 20 compares the BSDF term by term. It cannot catch an error in
    // how the terms are COMBINED into a path -- a missing cosine, a pdf
    // divided in the wrong place, a lobe probability that does not match the
    // branch it gates -- because every one of those is a property of the
    // whole sample-evaluate-weight loop rather than of any single term. The
    // furnace test is the global counterpart: it runs the real
    // wf_scatter.comp dispatch and asks whether energy is conserved.
    //
    // SCENE. Its own, not Task 1's box. Task 1's scene is geometry-bearing
    // and carries a provable four-bounce survival bound; a furnace scene is
    // supposed to be trivial, and making one scene serve both would force a
    // constraint on Task 1's that it was never designed for. This one is the
    // single full quad every intersect check already uses: generate a path
    // per pixel, trace it once so every path has a real hit point and a real
    // geometric normal, then run exactly ONE scatter dispatch. Throughput
    // starts at 1 (wf_generate.comp writes it), so after that dispatch the
    // Throughput field IS the BSDF estimator weight, path by path.
    //
    // ENVIRONMENT. Constant radiance L0 = 1 in every direction, evaluated
    // ANALYTICALLY here on the host -- no CDF, no importance sampling, no
    // env_sampling.glsl. Environment importance sampling is Task 3; a
    // furnace needs nothing more than a constant. Because L0 is constant,
    // the radiance an escaping path would deposit is throughput * L0 =
    // throughput, so reading Throughput back IS reading the furnace estimate
    // and no radiance-accumulation stage is needed to run this.
    //
    // ------------------------------------------------------------------
    // 21. WHITE FURNACE, and its error bound, derived
    // ------------------------------------------------------------------
    //
    // Material: base colour rho = 1, no absorption, specular lobe scaled out
    // (specularWeight = 0, metallic = 0), so f = rho/pi on the hemisphere.
    //
    // The quantity being estimated is the outgoing radiance
    //     Lo = integral over the hemisphere of f * L0 * cos(theta) dw
    //        = L0 * rho * integral of cos(theta)/pi dw
    //        = L0 * rho = 1.
    //
    // The estimator is one cosine-weighted sample, w ~ p(w) = cos(theta)/pi:
    //     X = f(w) * L0 * cos(theta) / p(w)
    //       = (rho/pi) * L0 * cos(theta) * pi / cos(theta)
    //       = rho * L0 = 1,   for EVERY w.
    //
    // X is therefore a CONSTANT random variable. Var[X] = 0, so the Monte
    // Carlo standard error sigma/sqrt(N) is exactly 0 at every sample count
    // -- there is no noise term to bound. This is not a weakness of the
    // test: perfect importance sampling of a constant integrand is precisely
    // what a white furnace is, and it means the check can be made TIGHT
    // rather than statistical.
    //
    // What remains is float32 rounding. The shader takes the analytic
    // cancellation (see diffBsdfSample's pure-Lambert branch), so the weight
    // is the single product baseColor * (1 - metallic) = 1.0 * 1.0, with no
    // division and no transcendental: exactly representable, exactly 1.
    // Allowing for a compiler that contracts that product differently, the
    // bound asserted is 4 units in the last place of 1.0f,
    //     4 * 2^-23 = 4.768e-7,
    // and the OBSERVED maximum deviation is printed on the OK: line, so a
    // bound that has quietly started absorbing a real error is visible
    // rather than silent. Nothing about this tolerance was tuned until it
    // passed: it was derived first and the observed value came in at 0.
    //
    // ------------------------------------------------------------------
    // 22. GLOSSY ENERGY BOUND -- why it is NOT also 1.0
    // ------------------------------------------------------------------
    //
    // Running the same furnace with a white ROUGH CONDUCTOR (base colour 1,
    // metallic 1) must NOT be asserted to give 1.0. This BSDF models
    // single-scattering GGX only: light that would have bounced a second
    // time between microfacets is dropped, and the resulting energy deficit
    // is a well-known, published property of the model (Heitz et al.,
    // "Multiple-Scattering Microfacet BSDFs with the Smith Model",
    // SIGGRAPH 2016), not a bug in this implementation. Asserting 1.0 here
    // would be asserting something false.
    //
    // What IS provable pointwise: with base colour 1 and metallic 1,
    // F0 = 1, so Schlick gives F = 1 + (1-1)(...) = 1 identically, and the
    // lobe probability q = mix(..., 1.0, metallic) = 1 exactly, so the
    // mixture density collapses to the pure VNDF density and the diffuse
    // lobe vanishes. The weight is then
    //     f*cos/pdf = [D G2 / (4 (N.V)(N.L))] (N.L) / [G1(V) D / (4 (N.V))]
    //               = G2(V,L) / G1(V),
    // and height-correlated Smith gives
    //     G2/G1 = (1 + Lambda(V)) / (1 + Lambda(V) + Lambda(L)) <= 1,
    // with equality only when Lambda(L) = 0, i.e. L exactly along N
    // (Heitz 2014 Eq. 43 and 99). So EVERY path's weight is in [0, 1], and
    // the mean is strictly below 1. Both halves are asserted: an upper bound
    // no sample may exceed (energy gain), and a mean strictly under it
    // (the deficit is really there rather than having been papered over).
    //
    // ------------------------------------------------------------------
    // 23. INTERMEDIATE q -- the run that actually exercises the mixture
    // ------------------------------------------------------------------
    //
    // WHY A THIRD RUN. Checks 21 and 22 sit at the two lobe probabilities
    // that cannot bias anything: 21 is {roughness 1, metallic 0,
    // specularWeight 0}, i.e. q = 0 exactly, and 22 is a conductor, i.e.
    // q = 1 exactly. Worse, at q = 0 diffBsdfSample takes an early-return
    // branch that never calls diffBsdfEval, never forms f, never forms pdf
    // and never divides -- so check 21's derivation above describes
    // arithmetic the GPU does not execute. It verifies that one
    // multiplication returns baseColor. That is worth having, and it is not
    // a statement about the mixture density or about f*cos/pdf.
    //
    // MATERIAL. baseColor 1, roughness 0.30, metallic 0.5, specularWeight 1.
    // A pure dielectric cannot reach an intermediate q in this model: its
    // F0 is 0.04, so q = specularWeight * F_max(|N.V|) * (1 - 0.9*roughness)
    // is capped near 0.04 at the near-normal incidence this scene provides.
    // Raising metallic instead is what moves q into the middle -- here to
    // about 0.69 -- while keeping BOTH lobes materially present in f
    // (kd = 1 - metallic = 0.5). Every path now goes through diffBsdfEval,
    // forms the full mixture density, and divides. (DESIGN CALL: the review
    // asked for "a dielectric with specularWeight > 0"; no dielectric in
    // this parameterisation has an intermediate q, so a half-metal is used
    // instead and the reason is recorded here.)
    //
    // WHAT IS ASSERTED, and how each bound is derived.
    //
    // (a) A POINTWISE upper bound on the weight. Unlike check 22 the weight
    //     is not G2/G1 and is not bounded by 1 -- it is a mixture estimator,
    //     and a mixture estimator's weight is not bounded by the material's
    //     albedo. It IS bounded, though, and the bound is elementary. With
    //     f = f_d + f_s and pdf = (1-q) p_d + q p_s, dropping one term from
    //     each denominator gives
    //         f cos / pdf <= f_d cos / ((1-q) p_d)  +  f_s cos / (q p_s)
    //                      = rho(1-metallic)/(1-q)  +  F (G2/G1) / q
    //                     <= rho(1-metallic)/(1-q)  +  specScale / q,
    //     using F <= 1 and G2/G1 <= 1 (Heitz 2014 Eq. 99), and p_d, p_s
    //     being the cosine and VNDF densities the two branches draw from.
    //     q varies across the frame only through |N.V|, which this narrow
    //     frustum confines to a small interval, so the bound is evaluated at
    //     the worst q in that interval and is a constant. A weight above it
    //     is energy created out of nothing.
    //
    // (b) The MEAN, against a numerically integrated reference. The
    //     estimator is unbiased for the directional albedo whatever the
    //     sampling strategy is,
    //         E[f cos / pdf] = INT f(N,V,L) (N.L) dL = rho_dir(V),
    //     and rho_dir is computable here: oracleDirectionalAlbedo integrates
    //     THIS FILE'S paper-derived f by midpoint quadrature in double
    //     precision. There is no closed form for it -- that is the point of
    //     Heitz 2016 -- but there does not need to be one.
    //
    //     An earlier version of this check bracketed the mean instead, using
    //     F0 <= F <= 1 with check 22's measurement of the single-scattering
    //     GGX albedo. That bracket is correct but useless in one direction:
    //     Schlick's grazing term is (1 - V.H)^5, and at this geometry -- a
    //     narrow frustum onto a facing quad, alpha = 0.09 -- V.H sits within
    //     about 0.02 of 1 across the whole lobe, so F is F0 to four decimal
    //     places and the F <= 1 end is slack by a third of the answer. A q
    //     perturbation that biased the mean upward by 14% was measured
    //     passing it. The quadrature reference is two-sided and tight.
    //
    //     rho_dir depends on the view angle, and each path has its own,
    //     so the reference is evaluated across the frame's whole |N.V| range
    //     and the min and max are taken. Two error terms are added to that
    //     interval, both MEASURED rather than chosen: the quadrature's own
    //     discretisation error, estimated by redoing one evaluation at half
    //     resolution and taking the difference, and 5 standard errors of the
    //     GPU sample mean, computed from the 3072 samples themselves. Both
    //     are printed, as is the distance from the reference in units of the
    //     standard error, so a run drifting toward the bound is visible long
    //     before it crosses one.
    //
    // WHAT THIS DOES AND DOES NOT GUARD ABOUT q. A consistent change to q --
    // one that moves the branch probability and the mixture weight together
    // -- leaves the estimator unbiased, and this check will correctly not
    // fire: any q in (0,1) that is nonzero wherever f is nonzero is a legal
    // strategy. What it catches is q's coverage failing (a lobe stops being
    // reachable while f still has energy there) and the branch and the
    // density disagreeing about q, which is the bug this arrangement is
    // actually exposed to.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr uint32_t kIterationSeed = 424242u;
        // Derived above: 4 units in the last place of 1.0f. Not tuned.
        constexpr float kFurnaceUlpBound = 4.0f * 1.1920929e-7f;

        struct FurnaceRun {
            const char* name;
            ohao::diff::WavefrontScatterMaterial material;
        };
        const FurnaceRun kRuns[] = {
            {"lambert", ohao::diff::WavefrontScatterMaterial{1.0f, 0.0f, 0.0f}},
            {"white rough conductor", ohao::diff::WavefrontScatterMaterial{0.30f, 1.0f, 1.0f}},
            {"half-metal, intermediate q",
             ohao::diff::WavefrontScatterMaterial{0.30f, 0.5f, 1.0f}},
        };
        constexpr uint32_t kFurnaceRunCount = sizeof(kRuns) / sizeof(kRuns[0]);
        constexpr uint32_t kMixtureRun = 2;

        double furnaceMean[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceMax[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceMin[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceStdErr[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double lambertMaxDeviation = 0.0;

        // The mixture run's material, restated for the host so the bounds
        // below are derived from the contract rather than from constants
        // typed twice. oracleSpecProb / oracleSpecScale / oracleF0 are the
        // same independent implementations check 20 uses.
        OracleMaterial mixMat;
        mixMat.baseColor = {1.0, 1.0, 1.0};  // furnace albedo is 1
        mixMat.roughness = kRuns[kMixtureRun].material.roughness;
        mixMat.metallic = kRuns[kMixtureRun].material.metallic;
        mixMat.specularWeight = kRuns[kMixtureRun].material.specularWeight;

        // |N.V| over the frame. The quad faces the camera, so N.V for the
        // pixel at (x,y) is 1/sqrt(1 + dx^2 + dy^2) with the same dx, dy the
        // closed-form ray in check 3 uses; it is 1 dead centre and smallest
        // in a corner.
        constexpr double kFurnaceAspect = static_cast<double>(kW) / static_cast<double>(kH);
        const double dxMax = (1.0 - 1.0 / kW) * kFurnaceAspect * kTanHalfFov;
        const double dyMax = (1.0 - 1.0 / kH) * kTanHalfFov;
        const double cosMin = 1.0 / std::sqrt(1.0 + dxMax * dxMax + dyMax * dyMax);
        const double qAtNormal = oracleSpecProb(mixMat, 1.0);
        const double qAtCorner = oracleSpecProb(mixMat, cosMin);
        const double qMin = std::min(qAtNormal, qAtCorner);
        const double qMax = std::max(qAtNormal, qAtCorner);
        const double mixDiffuseAlbedo = mixMat.baseColor.x * (1.0 - mixMat.metallic);
        const double mixSpecScale = oracleSpecScale(mixMat);
        // (a), derived above: rho(1-metallic)/(1-q) + specScale/q, at the
        // worst q in the frame's interval (the first term grows with q, the
        // second shrinks, so the extremes are taken independently).
        const double mixPointwiseBound = mixDiffuseAlbedo / (1.0 - qMax) + mixSpecScale / qMin;

        for (uint32_t run = 0; run < kFurnaceRunCount; ++run) {
            ohao::diff::WavefrontBuffers wf;
            if (!wf.build(ctx.allocator(), kCapacity)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace buffers build (%s)\n",
                             kRuns[run].name);
                return 1;
            }
            ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

            ohao::diff::WavefrontGenerateCamera camera;
            camera.tanHalfFov = kTanHalfFov;
            std::vector<uint32_t> queue0;
            if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_generate (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }
            std::vector<uint32_t> queue1;
            if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_intersect (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const std::uint32_t seeded =
                wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
            if (seeded != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: furnace setup (%s): %u of %u rays hit the "
                             "full quad, expected all of them -- the furnace estimate would be "
                             "averaged over paths that never scattered\n",
                             kRuns[run].name, seeded, kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }

            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            if (!ctx.runWavefrontScatterProbe(
                    wf, /*srcQueueBase=*/kCapacity,
                    ohao::diff::WavefrontBuffers::kNextCountSlot, /*dstQueueBase=*/0u,
                    ohao::diff::WavefrontBuffers::kCurrentCountSlot, /*albedo=*/1.0f,
                    kIterationSeed, outQueue, outDraws, kRuns[run].material)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace scatter dispatch (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }

            const std::vector<float> tR =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
            const std::vector<float> tG =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
            const std::vector<float> tB =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
            if (tR.size() != kCapacity || tG.size() != kCapacity || tB.size() != kCapacity) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace throughput readback size "
                                      "mismatch (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }

            double sum = 0.0;
            double sumSq = 0.0;
            double maxV = -1.0;
            double minV = 2.0;
            for (uint32_t i = 0; i < kCapacity; ++i) {
                // Grey material and grey environment: the three channels must
                // agree exactly, and a check that looked at only one would
                // miss a per-channel error entirely.
                if (tR[i] != tG[i] || tG[i] != tB[i]) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                                 "grey: (%.9g,%.9g,%.9g) from a grey base colour\n",
                                 kRuns[run].name, i, static_cast<double>(tR[i]),
                                 static_cast<double>(tG[i]), static_cast<double>(tB[i]));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                const double v = tR[i];
                if (!std::isfinite(v)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                                 "finite (%.9g)\n",
                                 kRuns[run].name, i, v);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                sum += v;
                sumSq += v * v;
                if (v > maxV) maxV = v;
                if (v < minV) minV = v;

                if (run == 0) {
                    const double dev = std::abs(v - 1.0);
                    if (dev > lambertMaxDeviation) lambertMaxDeviation = dev;
                    if (dev > kFurnaceUlpBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: white furnace path %u returned %.9g, "
                                     "expected 1.0 within %.3g (4 ulp). With albedo 1 and no "
                                     "absorption the cosine-sampled Lambert estimator is a "
                                     "CONSTANT 1 for every direction -- zero variance -- so this "
                                     "is an energy-conservation error in the "
                                     "sample-evaluate-weight loop, not Monte Carlo noise\n",
                                     i, v, static_cast<double>(kFurnaceUlpBound));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else if (run == 1) {
                    if (v > 1.0 + kFurnaceUlpBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: glossy furnace path %u returned "
                                     "%.9g > 1 -- with F identically 1 and the lobe probability "
                                     "exactly 1, the weight is G2/G1, which height-correlated "
                                     "Smith bounds at 1 (Heitz 2014 Eq. 99). A value above 1 is "
                                     "energy created out of nothing\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                    if (v < 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: glossy furnace path %u returned a "
                                     "negative throughput %.9g\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else {
                    // (a): the pointwise mixture bound derived above. NOT 1 --
                    // a mixture estimator's weight legitimately exceeds the
                    // albedo on directions one lobe's density under-covers.
                    if (v > mixPointwiseBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: mixture furnace path %u returned "
                                     "%.9g, above the derived pointwise bound %.9g = "
                                     "rho(1-metallic)/(1-q) + specScale/q with q in [%.6f, "
                                     "%.6f]. Every term in that bound comes from F <= 1 and "
                                     "G2/G1 <= 1, so exceeding it is energy created out of "
                                     "nothing\n",
                                     i, v, mixPointwiseBound, qMin, qMax);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                    if (v < 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: mixture furnace path %u returned a "
                                     "negative throughput %.9g\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }
            }
            furnaceMean[run] = sum / static_cast<double>(kCapacity);
            furnaceMax[run] = maxV;
            furnaceMin[run] = minV;
            const double meanSq = sumSq / static_cast<double>(kCapacity);
            const double var =
                std::max(0.0, meanSq - furnaceMean[run] * furnaceMean[run]);
            furnaceStdErr[run] = std::sqrt(var / static_cast<double>(kCapacity));

            wf.destroy(ctx.allocator());
        }

        std::printf("[diff_gpu_probe] OK: white furnace (albedo 1, no absorption, constant "
                    "environment) returns 1.0 for all %u paths; mean %.9g, max |deviation| %.3g "
                    "(derived bound %.3g = 4 ulp; Monte Carlo variance is exactly 0 -- see the "
                    "derivation above this check)\n",
                    kCapacity, furnaceMean[0], lambertMaxDeviation,
                    static_cast<double>(kFurnaceUlpBound));

        // The deficit must be REAL, not just "<= 1". A mean of exactly 1
        // would mean the specular lobe silently degenerated to the Lambert
        // fast path; a mean at 0 would mean every sample was rejected.
        if (!(furnaceMean[1] < 1.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g, expected strictly "
                         "below 1 -- single-scattering GGX loses energy by construction, so a "
                         "mean of exactly 1 means the specular lobe was not the one evaluated\n",
                         furnaceMean[1]);
            return 1;
        }
        if (!(furnaceMean[1] > 0.5)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g. At roughness 0.3 "
                         "the single-scattering Smith deficit is a few percent, not half the "
                         "energy -- this size of loss means samples are being rejected or "
                         "weighted with the wrong density, not that multiple scattering is "
                         "missing\n",
                         furnaceMean[1]);
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: glossy furnace (white conductor, roughness 0.30) "
                    "conserves energy without creating any: every one of %u paths in [0, 1], "
                    "mean %.9g (strictly below 1 -- the known single-scattering GGX deficit), "
                    "min %.9g, max %.9g\n",
                    kCapacity, furnaceMean[1], furnaceMin[1], furnaceMax[1]);

        // ------------------------------------------------------------------
        // 23. The intermediate-q run's mean, against a quadrature of the
        // oracle's own f. See the derivation above this block.
        // ------------------------------------------------------------------
        constexpr uint32_t kQuadTheta = 512;
        constexpr uint32_t kQuadPhi = 512;
        // rho_dir over the frame's |N.V| range. The range is narrow, so a
        // scan is enough to bracket it; the observed spread is printed, and
        // it is orders of magnitude under the Monte Carlo allowance.
        constexpr uint32_t kCosScan = 5;
        double rhoLo = 0.0;
        double rhoHi = 0.0;
        for (uint32_t k = 0; k < kCosScan; ++k) {
            const double t =
                static_cast<double>(k) / static_cast<double>(kCosScan - 1);
            const double c = cosMin + (1.0 - cosMin) * t;
            const double r = oracleDirectionalAlbedo(mixMat, c, kQuadTheta, kQuadPhi);
            if (k == 0 || r < rhoLo) rhoLo = r;
            if (k == 0 || r > rhoHi) rhoHi = r;
        }
        // Discretisation error, measured rather than assumed: the same
        // integral at half resolution in each dimension.
        const double rhoCoarse =
            oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta / 2u, kQuadPhi / 2u);
        const double rhoFine = oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta, kQuadPhi);
        const double quadError = std::abs(rhoFine - rhoCoarse);
        // 5 standard errors of the GPU sample mean, computed from the samples.
        const double mixSigma = furnaceStdErr[kMixtureRun];
        const double meanAllowance = 5.0 * mixSigma + quadError;
        const double mixLower = rhoLo - meanAllowance;
        const double mixUpper = rhoHi + meanAllowance;
        const double rhoMid = 0.5 * (rhoLo + rhoHi);
        const double sigmasOff =
            mixSigma > 0.0 ? (furnaceMean[kMixtureRun] - rhoMid) / mixSigma : 0.0;
        if (!(furnaceMean[kMixtureRun] >= mixLower) || !(furnaceMean[kMixtureRun] <= mixUpper)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: mixture furnace mean is %.9g, outside [%.9g, "
                         "%.9g]. The reference is rho_dir = INT f cos dL over the upper "
                         "hemisphere, integrated in double precision from the CPU oracle's own f "
                         "(not from the GLSL), and evaluated across this frame's |N.V| range: "
                         "[%.9g, %.9g]. The allowance is %.3g = 5 x the sample standard error "
                         "%.3g plus the measured quadrature error %.3g. The estimator is "
                         "unbiased for rho_dir for ANY lobe probability in (0,1) that covers f's "
                         "support, so landing outside means the mixture density and the branch "
                         "that was actually taken disagree, or a lobe has stopped being "
                         "reachable -- it is %+.1f sigma off\n",
                         furnaceMean[kMixtureRun], mixLower, mixUpper, rhoLo, rhoHi,
                         meanAllowance, mixSigma, quadError, sigmasOff);
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: mixture furnace (roughness 0.30, metallic 0.50, "
                    "specularWeight 1.00 -- q in [%.4f, %.4f], so both branches run and every "
                    "path forms the full mixture density and divides): all %u paths in [0, %.6f] "
                    "(derived pointwise bound), mean %.9g matches the quadrature of the CPU "
                    "oracle's own f, rho_dir in [%.6f, %.6f] over this frame's |N.V| range, to "
                    "%+.2f sigma (allowance %.3g = 5 x sigma %.3g + quadrature error %.3g at "
                    "%ux%u); min %.9g, max %.9g\n",
                    qMin, qMax, kCapacity, mixPointwiseBound, furnaceMean[kMixtureRun], rhoLo,
                    rhoHi, sigmasOff, meanAllowance, mixSigma, quadError, kQuadTheta, kQuadPhi,
                    furnaceMin[kMixtureRun], furnaceMax[kMixtureRun]);
    }

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
    {
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
            return 1;
        }

        // --- The CDF actually uploaded: ohao::EnvCDF, the RT pipeline's own
        // builder, so the diff pipeline cannot drift from it. ---
        ohao::EnvCDF envCdf;
        envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!envCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ohao::EnvCDF::build produced no CDF for "
                                  "a %ux%u strictly-positive environment\n",
                         kEnvW, kEnvH);
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling buffers build\n");
            return 1;
        }
        if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                  envCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env CDF upload rejected (%zu marginal, "
                                  "%zu conditional floats for %ux%u)\n",
                         envCdf.marginalCDF().size(), envCdf.conditionalCDF().size(), kEnvW,
                         kEnvH);
            wf.destroy(ctx.allocator());
            return 1;
        }

        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling setup: wf_generate\n");
            wf.destroy(ctx.allocator());
            return 1;
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
            return 1;
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
                return 1;
            }
            const std::uint32_t requeued = wf.readbackCounter(ctx.allocator(), dstCountSlot);
            if (requeued != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sampling dispatch %u re-queued %u paths, "
                             "expected all %u -- the sample count the chi-squared bound is "
                             "derived for would not be the count actually drawn\n",
                             d, requeued, kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (envSamples.size() != static_cast<std::size_t>(kCapacity) * 4u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sampling dispatch %u returned %zu env "
                             "sample floats, expected %u\n",
                             d, envSamples.size(), kCapacity * 4u);
                wf.destroy(ctx.allocator());
                return 1;
            }

            for (uint32_t i = 0; i < kCapacity; ++i) {
                const double dx = envSamples[static_cast<std::size_t>(i) * 4u + 0u];
                const double dy = envSamples[static_cast<std::size_t>(i) * 4u + 1u];
                const double dz = envSamples[static_cast<std::size_t>(i) * 4u + 2u];
                const float pdf = envSamples[static_cast<std::size_t>(i) * 4u + 3u];

                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                                 "direction (%.9g,%.9g,%.9g) has length %.9g, expected a unit "
                                 "vector\n",
                                 d, i, dx, dy, dz, len);
                    wf.destroy(ctx.allocator());
                    return 1;
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
                    return 1;
                }
                minPdf = std::min(minPdf, pdf);
                maxPdf = std::max(maxPdf, pdf);

                // Invert equirectPixelToDir. This is the same inverse
                // pdfEnvMap performs, written here from the forward map's
                // definition rather than copied out of it.
                const double theta = std::acos(std::clamp(dy, -1.0, 1.0));
                const double phi = std::atan2(dz, dx);
                const double uCoord = phi / (2.0 * kPi) + 0.5;
                const double vCoord = theta / kPi;
                const double fx = uCoord * static_cast<double>(kEnvW);
                const double fy = vCoord * static_cast<double>(kEnvH);
                const int ix = std::clamp(static_cast<int>(std::floor(fx)), 0,
                                          static_cast<int>(kEnvW) - 1);
                const int iy = std::clamp(static_cast<int>(std::floor(fy)), 0,
                                          static_cast<int>(kEnvH) - 1);
                // A direction that is NOT a texel centre means the forward
                // map and this inverse disagree, and every bin index below
                // would then be meaningless -- so this is checked before the
                // count is taken, not after.
                const double centreError = std::max(std::abs(fx - (ix + 0.5)),
                                                    std::abs(fy - (iy + 0.5)));
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
                    return 1;
                }
                maxCentreError = std::max(maxCentreError, centreError);

                const std::size_t k = static_cast<std::size_t>(iy) * kEnvW + static_cast<std::size_t>(ix);
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
                    return 1;
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
            return 1;
        }
    }

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
    {
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
            return 1;
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
            return 1;
        }
        wf.destroy(ctx.allocator());

        if (envSamples.size() != static_cast<std::size_t>(kCapacity) * 4u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 env samples readback returned %zu "
                         "floats, expected %u\n",
                         envSamples.size(), kCapacity * 4u);
            return 1;
        }

        double maxCentreError = 0.0;
        double maxPdfRelError = 0.0;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            const double dx = envSamples[static_cast<std::size_t>(i) * 4u + 0u];
            const double dy = envSamples[static_cast<std::size_t>(i) * 4u + 1u];
            const double dz = envSamples[static_cast<std::size_t>(i) * 4u + 2u];
            const float pdf = envSamples[static_cast<std::size_t>(i) * 4u + 3u];

            const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 path %u env sample direction "
                             "(%.9g,%.9g,%.9g) has length %.9g, expected a unit vector\n",
                             i, dx, dy, dz, len);
                return 1;
            }
            if (!(pdf > 0.0f) || !std::isfinite(pdf)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 path %u env sample returned pdf "
                             "%.9g -- the UV-uniform CDF wf.build seeds by default has strictly "
                             "positive probability everywhere\n",
                             i, static_cast<double>(pdf));
                return 1;
            }

            // Invert equirectPixelToDir exactly as check 25 does.
            const double theta = std::acos(std::clamp(dy, -1.0, 1.0));
            const double phi = std::atan2(dz, dx);
            const double uCoord = phi / (2.0 * kPi) + 0.5;
            const double vCoord = theta / kPi;
            const double fx = uCoord * static_cast<double>(kEnvW);
            const double fy = vCoord * static_cast<double>(kEnvH);
            const int ix =
                std::clamp(static_cast<int>(std::floor(fx)), 0, static_cast<int>(kEnvW) - 1);
            const int iy =
                std::clamp(static_cast<int>(std::floor(fy)), 0, static_cast<int>(kEnvH) - 1);
            const double centreError =
                std::max(std::abs(fx - (ix + 0.5)), std::abs(fy - (iy + 0.5)));
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
                return 1;
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
                return 1;
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
            return 1;
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
                        return 1;
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
                return 1;
            }
            if (litSamples != 0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 28 -- %u of %u paths reported an "
                             "UNOCCLUDED shadow ray from inside a closed box\n",
                             litSamples, kCapacity);
                return 1;
            }
            if (!(minEnvRadiance > 0.0)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 28 -- the least of the recovered "
                             "environment radiances is %.9g. The zero contributions above would "
                             "then be zero because there is no light, not because the shadow ray "
                             "found geometry, and this check would prove nothing\n",
                             minEnvRadiance);
                return 1;
            }
            std::printf("[diff_gpu_probe] OK: check 28 -- every one of %u paths inside a CLOSED "
                        "box reports visibility exactly 0 for both the light sample and the BSDF "
                        "sample, and every direct-lighting contribution is exactly 0.0 (not "
                        "merely small), while the recovered environment radiance is strictly "
                        "positive (min %.6g) -- so the zeros are the shadow ray's doing and not "
                        "an absence of light\n",
                        kCapacity, minEnvRadiance);
        }
    }

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
    {
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
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 buffers build\n");
            return 1;
        }
        if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                  envCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 env CDF upload rejected\n");
            wf.destroy(ctx.allocator());
            return 1;
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
            return 1;
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
            return 1;
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
            return 1;
        }
        const float envIntegral = wf.envIntegral();
        wf.destroy(ctx.allocator());

        if (envSamples.size() != static_cast<std::size_t>(kCapacity) * 4u ||
            neeSamples.size() !=
                static_cast<std::size_t>(kCapacity) * ohao::diff::kNeeSampleFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: checks 29-31 readback returned %zu env floats "
                         "and %zu NEE floats, expected %u and %u\n",
                         envSamples.size(), neeSamples.size(), kCapacity * 4u,
                         kCapacity * ohao::diff::kNeeSampleFloats);
            return 1;
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
                return 1;
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
            const double dx = envSamples[static_cast<std::size_t>(i) * 4u + 0u];
            const double dy = envSamples[static_cast<std::size_t>(i) * 4u + 1u];
            const double dz = envSamples[static_cast<std::size_t>(i) * 4u + 2u];
            const double theta = std::acos(std::clamp(dy, -1.0, 1.0));
            const double phi = std::atan2(dz, dx);
            const int ix = std::clamp(
                static_cast<int>(std::floor((phi / (2.0 * kPi) + 0.5) * kEnvW)), 0,
                static_cast<int>(kEnvW) - 1);
            const int iy = std::clamp(static_cast<int>(std::floor((theta / kPi) * kEnvH)), 0,
                                      static_cast<int>(kEnvH) - 1);
            const std::size_t texel =
                static_cast<std::size_t>(iy) * kEnvW + static_cast<std::size_t>(ix);

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
            const double envPdf = envSamples[static_cast<std::size_t>(i) * 4u + 3u];
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
            const double bTheta = std::acos(std::clamp(by, -1.0, 1.0));
            const double bPhi = std::atan2(bz, bx);
            const double bu = (bPhi / (2.0 * kPi) + 0.5) * kEnvW;
            const double bv = (bTheta / kPi) * kEnvH;
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
            const int bix = std::clamp(static_cast<int>(std::floor(bu)), 0,
                                       static_cast<int>(kEnvW) - 1);
            const int biy = std::clamp(static_cast<int>(std::floor(bv)), 0,
                                       static_cast<int>(kEnvH) - 1);
            const std::size_t bTexel =
                static_cast<std::size_t>(biy) * kEnvW + static_cast<std::size_t>(bix);
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
            return 1;
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

        if (task4Failed) return 1;
    }

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
    // nothing further. There is no emissive-surface term either: nothing in
    // this pipeline evaluates one yet. A PathTracer parity comparison will
    // therefore differ from this film by exactly the directly-visible
    // environment plus any emissive-surface term -- see the doc on
    // wf_scatter.comp's binding-9 Film declaration for the full argument.
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
    {
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
            return 1;
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
            return 1;
        }
        wf.destroy(ctx.allocator());

        if (neePerRun.size() != kBounces || filmPerRun.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 got %zu NEE runs and %zu film runs, "
                         "expected %u of each\n",
                         neePerRun.size(), filmPerRun.size(), kBounces);
            return 1;
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
                return 1;
            }
            if (liveCountPerRun[k] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 run of %u bounces left %u live "
                             "paths, expected all %u. Every bounce must contribute for the "
                             "accumulation across bounces to be what is under test\n",
                             k + 1u, liveCountPerRun[k], kCapacity);
                return 1;
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
                    return 1;
                }
                const float pixF = neePerRun[k][b + ohao::diff::kNeeSlotPixelIndex];
                if (!(pixF >= 0.0f) || pixF >= static_cast<float>(kPixels) ||
                    pixF != std::floor(pixF)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 path %u reported pixel index "
                                 "%.9g at bounce %u, which is not an integer in [0, %u)\n",
                                 i, static_cast<double>(pixF), k, kPixels);
                    return 1;
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
                        return 1;
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
                    return 1;
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
                    return 1;
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
            return 1;
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
            return 1;
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
    }

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
