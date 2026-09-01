// The independent CPU BSDF oracle (Stage 0b-2b Task 2, check 20).
//
// Lifted verbatim out of diff_gpu_probe.cpp: this is the same code, with the
// same provenance commentary, in its own translation unit. The types and the
// two shared constants it used to declare at file scope now live in
// oracle_bsdf.hpp, because ties.cpp needs them across a module boundary --
// a linkage change, not a value change.
#include "probe/oracle_bsdf.hpp"

#include <algorithm>
#include <cmath>

namespace ohao::diff::probe {

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

/// One direction binned back to an equirectangular texel, HOST-SIDE.
///
/// WHY THIS IS ONE FUNCTION (whole-branch review finding). This inversion --
/// theta = acos(y), phi = atan2(z, x), floor(u*W)/floor(v*H) with the same
/// clamps -- was hand-written FIVE times in this file: the parity
/// reference's environment lookup and one copy each in checks 25, 27 and 31
/// (twice, once per sampled direction). Checks 25, 27, 31 and 33/34 all rest
/// on those five agreeing. env_sampling.glsl factored the SHADER's two
/// copies into `envTexelPdfUV` on this same branch, for exactly this reason
/// -- "a change to the binning cannot reach one of them and not the other" --
/// and the host side got the opposite treatment in the same stage.
///
/// It stays INDEPENDENT of the shader: this is written from the forward map
/// (equirectPixelToDir) inverted by hand, in double, and is an ORACLE for
/// what the GPU produced. Single-sourcing it on the host does not make it
/// share anything with the GPU; it makes the five host copies one.
OracleEnvTexel oracleEnvTexelOf(double dx, double dy, double dz, std::uint32_t envW,
                                std::uint32_t envH) {
    OracleEnvTexel out;
    out.theta = std::acos(std::clamp(dy, -1.0, 1.0));
    out.phi = std::atan2(dz, dx);
    out.fx = (out.phi / (2.0 * kOraclePi) + 0.5) * static_cast<double>(envW);
    out.fy = (out.theta / kOraclePi) * static_cast<double>(envH);
    out.ix = std::clamp(static_cast<int>(std::floor(out.fx)), 0, static_cast<int>(envW) - 1);
    out.iy = std::clamp(static_cast<int>(std::floor(out.fy)), 0, static_cast<int>(envH) - 1);
    out.index = static_cast<std::size_t>(out.iy) * envW + static_cast<std::size_t>(out.ix);
    out.centreError = std::max(std::abs(out.fx - (out.ix + 0.5)),
                               std::abs(out.fy - (out.iy + 0.5)));
    return out;
}

/// Relative difference that degrades gracefully to absolute near zero. f and
/// pdf span many orders of magnitude across the case table (a sharp GGX
/// lobe's D is ~10^3 at roughness 0.1 and ~10^-1 at roughness 0.8), so a
/// purely absolute tolerance would be meaningless at one end and vacuous at
/// the other.
double oracleRelDiff(double reference, double measured) {
    const double denom = std::max(1e-6, std::abs(reference));
    return std::abs(measured - reference) / denom;
}


}  // namespace ohao::diff::probe
