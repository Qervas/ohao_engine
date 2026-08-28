#ifndef OHAO_DIFF_BSDF_GLSL
#define OHAO_DIFF_BSDF_GLSL

// The wavefront path tracer's surface BSDF: a Lambertian diffuse lobe plus a
// single-scattering GGX microfacet specular lobe, with ONE stochastic lobe
// choice per bounce (no MIS -- that is Stage 0b-2b Task 4).
//
// This file is the single definition of the model. wf_scatter.comp calls it
// to advance a path; shaders/diff/bsdf_probe.comp calls the same two entry
// points so diff_gpu_probe.cpp's check 20 can compare f, the pdf and the
// sampler's weight against a CPU oracle written from the published formulas.
// There is deliberately no second copy of any of this: this project has been
// bitten three times by duplicated GPU code (camera_ray.glsl, loadSpv, and
// one barrier that ended up with three hand-maintained copies).
//
// The microfacet terms themselves are NOT re-implemented here either. D, the
// Smith Lambda/G1/G2 auxiliaries, the VNDF sampler and the tangent-basis
// helper all come from shaders/includes/material/ggx_aniso.glsl, the same
// file the RT path tracer's raygen shaders use, so the two pipelines cannot
// drift apart in their microfacet math.
//
// ---------------------------------------------------------------------------
// THE MODEL, stated as a contract
// ---------------------------------------------------------------------------
//
// Inputs. N is the forward-facing shading normal; V points AWAY from the
// surface toward the viewer (for a path tracer, V = -rayDirection); L points
// away from the surface toward the incoming light. All unit length.
// `baseColor`, `roughness` and `metallic` are the usual metal-rough triple;
// `roughness` is expected to have been through pbr_unpack.glsl's
// unpackHitPbr already (which floors it at 0.01), and alpha = roughness^2 --
// the convention ggxDiso and sampleGGXVNDF share.
//
// `specularWeight` scales the DIELECTRIC specular lobe: both its
// contribution to f and its share of the lobe-selection probability. It is
// the OpenPBR / Disney "specular" knob, and it exists here for a concrete
// reason: at specularWeight = 0 the model degenerates EXACTLY to
// f = baseColor/pi sampled by a cosine hemisphere, whose estimator weight
// f*cos/pdf is exactly `baseColor` with no rounding at all (see the
// float-exactness note on diffBsdfSample). That is the configuration the
// wavefront probes' constant-albedo throughput assertions rest on, so the
// BSDF has to be able to reach it exactly rather than approximately.
// Conductors are unaffected -- a metal has no diffuse lobe to fall back to,
// so specScale forces to 1 at metallic = 1.
//
//   F0        = mix(vec3(0.04), baseColor, metallic)      [Karis 2013]
//   specScale = mix(specularWeight, 1.0, metallic)        [contract]
//   f_diffuse = baseColor * (1 - metallic) / pi           [Lambert]
//   f_spec    = specScale * D(H) * F(V.H) * G2(V,L)
//               / (4 (N.V) (N.L))                         [Walter 2007 Eq.20]
//   f         = f_diffuse + f_spec
//
// Sampling strategy (a strategy, not physics -- any q in (0,1) that is
// nonzero wherever f is nonzero gives an unbiased estimator; this particular
// q is the one pt_raygen.rgen uses, scaled by specScale):
//
//   q     = clamp(mix(specScale * max(F(|N.V|)) * (1 - 0.9*roughness),
//                     1.0, metallic), 0, 1)
//   pdf   = (1-q) * (N.L)/pi  +  q * G1(V) D(H) / (4 (N.V))
//                                        [cosine pdf; Heitz 2018 VNDF pdf
//                                         folded with Walter 2007's
//                                         reflection Jacobian 1/(4 V.H)]
//
// Note that f and pdf are the FULL mixture on both sides: the sampler
// returns f*cos/pdf with the complete f and the complete mixture density,
// rather than the per-lobe "f_lobe/prob_lobe" shortcut pt_raygen.rgen uses.
// That shortcut is only unbiased when each lobe's sampler covers its own
// lobe exactly; the mixture form is unbiased regardless and is what makes
// the furnace test (check 21) a statement about the whole loop.
//
// NOT IN THIS MODEL, on purpose: no delta/mirror branch at very low
// roughness (the pdf of a snapped mirror direction is not the density it was
// drawn from, and reconciling that needs the delta-lobe bookkeeping MIS
// brings in Task 4); no multiple-scattering energy compensation, so a
// glossy white furnace correctly lands BELOW 1 -- see check 22; no
// anisotropy (ggxD_anisoOrIso's aniso branch is available but nothing in the
// wavefront path state carries a tangent frame yet).

#include "material/ggx_aniso.glsl"

const float DIFF_BSDF_PI = 3.14159265358979323846;
const float DIFF_BSDF_INV_PI = 0.31830988618379067154;

// Below this cosine a view or light direction is treated as grazing and the
// specular math -- which divides by (N.V) twice over -- is not evaluated.
// Every probe case sits orders of magnitude above it, so it is a guard
// against NaN in production, not a term any check measures.
const float DIFF_BSDF_MIN_COS = 1e-4;

// Cosine-weighted hemisphere sample about `normal` via Malley's method.
// (Pharr, Jakob & Humphreys, "Physically Based Rendering" 4th ed., A.5.3.)
//
// Moved here verbatim from wf_scatter.comp, where it was the whole of the
// Stage 0b-1 placeholder sampler. Its numerics must not drift: the fused
// loop's every-path-survives theorem depends on the strict positivity
// argued below.
//
// `normal` is an arbitrary unit vector, so the up-vector choice matters: it
// picks (0,0,1) unless the normal is within ~2.6 degrees of the z axis, in
// which case it falls back to (1,0,0). Either way `up` is far from parallel
// to `normal`, so the cross product is well conditioned and the resulting
// (tangent, bitangent, normal) frame is orthonormal for every normal. The
// returned direction satisfies dot(result, normal) = sqrt(1 - u1) >= 2^-12
// > 0 -- strictly inside the hemisphere, never grazing to zero -- because
// diffRngNext1D returns (state >> 8) * 2^-24 and so cannot exceed
// 1 - 2^-24.
vec3 diffCosineHemisphere(vec3 normal, float u1, float u2) {
    const float r = sqrt(u1);
    const float phi = 6.283185307179586 * u2;
    const float x = r * cos(phi);
    const float y = r * sin(phi);
    const float z = sqrt(max(0.0, 1.0 - u1));

    const vec3 up = (abs(normal.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tangent = normalize(cross(up, normal));
    const vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * x + bitangent * y + normal * z);
}

vec3 diffBsdfF0(vec3 baseColor, float metallic) {
    return mix(vec3(0.04), baseColor, metallic);
}

// Metals always carry a full specular lobe; dielectrics scale theirs.
float diffBsdfSpecScale(float specularWeight, float metallic) {
    return mix(specularWeight, 1.0, metallic);
}

vec3 diffBsdfSchlick(vec3 f0, float cosTheta) {
    const float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    const float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// Lobe-selection probability. Exactly 0 when the specular lobe is scaled
// out (specularWeight = 0, metallic = 0) and exactly 1 for a conductor --
// both of those exactness properties are load-bearing, see the header.
float diffBsdfSpecProb(vec3 N, vec3 V, vec3 baseColor, float roughness, float metallic,
                       float specularWeight) {
    const vec3 F = diffBsdfSchlick(diffBsdfF0(baseColor, metallic),
                                   clamp(abs(dot(N, V)), 0.0, 1.0));
    float q = diffBsdfSpecScale(specularWeight, metallic) * max(F.r, max(F.g, F.b)) *
              (1.0 - roughness * 0.9);
    q = mix(q, 1.0, metallic);
    return clamp(q, 0.0, 1.0);
}

/// f(N, V, L) and the density this file's sampler would have drawn L with.
void diffBsdfEval(vec3 N, vec3 V, vec3 L, vec3 baseColor, float roughness, float metallic,
                  float specularWeight, out vec3 f, out float pdf) {
    f = vec3(0.0);
    pdf = 0.0;

    const float NdotL = dot(N, L);
    const float NdotV = dot(N, V);
    // DELIBERATELY NOT `<= 0.0`. diff_gpu_probe.cpp's CPU oracle rejects at
    // 0, because the physics does: f is nonzero for every direction strictly
    // above the horizon. This rejects at 1e-4 instead, because the specular
    // branch below divides by (N.V) and by (N.L) and would return a
    // meaningless value in the band (0, 1e-4]. The two thresholds therefore
    // disagree, on purpose, over that band. It is a loud disagreement rather
    // than a silent one -- a sample landing there is a zero weight the oracle
    // calls valid -- so check 20 names the band explicitly (kShaderGrazingCos)
    // and counts such cases instead of failing on them. Every case in that
    // table sits orders of magnitude above the band, so the count is 0; if it
    // ever is not, the band is being entered and this comment is where to
    // start.
    if (NdotL <= DIFF_BSDF_MIN_COS || NdotV <= DIFF_BSDF_MIN_COS) return;

    // --- Lambertian diffuse. Metals have none. ---
    // Written as (baseColor * kd) * INV_PI and (NdotL * INV_PI) so that with
    // kd = 1 the numerator of f*cos/pdf is baseColor scaled by the SAME
    // rounded product NdotL*INV_PI that forms the denominator; see
    // diffBsdfSample.
    f = (baseColor * (1.0 - metallic)) * DIFF_BSDF_INV_PI;
    pdf = NdotL * DIFF_BSDF_INV_PI;

    const float q = diffBsdfSpecProb(N, V, baseColor, roughness, metallic, specularWeight);
    if (q <= 0.0) return;

    // --- Single-scattering GGX microfacet specular. ---
    const float alpha = roughness * roughness;
    const vec3 H = normalize(V + L);
    const float NdotH = max(dot(N, H), 0.0);
    const float VdotH = max(dot(V, H), 0.0);

    const float D = ggxDiso(NdotH, alpha);
    // Height-correlated Smith G2 (Heitz 2014 Eq. 99), built from
    // ggx_aniso.glsl's Lambda so the masking model cannot drift from the one
    // smithG1GGX/smithG2overG1GGX use.
    const float G2 = 1.0 / (1.0 + smithLambdaGGX(NdotV, alpha) + smithLambdaGGX(NdotL, alpha));
    const vec3 F = diffBsdfSchlick(diffBsdfF0(baseColor, metallic), VdotH);
    const float specScale = diffBsdfSpecScale(specularWeight, metallic);

    f += (specScale * D * G2 / (4.0 * NdotV * NdotL)) * F;

    // VNDF pdf (Heitz 2018 Eq. 3) with the reflection Jacobian 1/(4 V.H)
    // folded in; the (V.H) factors cancel, leaving G1(V) D / (4 N.V).
    const float pdfSpec = smithG1GGX(NdotV, alpha) * D / (4.0 * NdotV);
    pdf = mix(pdf, pdfSpec, q);
}

/// Draws L and returns the estimator weight f(L) * (N.L) / pdf(L), together
/// with the density `pdf` it was drawn with.
///
/// `uDir` is the 2-D sample both lobes consume; `uLobe` is the 1-D lobe
/// choice. The draw ORDER at the call site (uDir first, uLobe second) is
/// what keeps wf_scatter.comp's debug sink recording the same first two
/// values it always did.
///
/// L IS THE DIRECTION ACTUALLY DRAWN, including the GGX VNDF's below-horizon
/// tail, where the BRDF is zero and this returns `weight` = 0 and `pdf` = 0.
/// It is deliberately NOT replaced with something usable here. Substituting
/// a different direction is a path-continuation decision (the integrator
/// still needs a ray to trace, and one that keeps the path inside the
/// scene), not a BSDF decision, and folding it in here would make a rejected
/// sample indistinguishable from an accepted one to anything downstream --
/// including diff_gpu_probe.cpp's check 20, which can only confirm that a
/// zero weight was LEGITIMATE by evaluating its own oracle at the direction
/// that was really drawn. wf_scatter.comp does the substitution.
///
/// PURE-LAMBERT FAST PATH, and why it is not an optimisation. When the
/// specular lobe is scaled entirely out (q == 0), the estimator weight for a
/// cosine-weighted sample collapses analytically:
///
///     f*cos/pdf = (rho/pi * cos) / (cos/pi) = rho,      exactly, for all L.
///
/// Taking that closed form is the difference between an EXACT answer and an
/// almost-exact one. GPU compilers lower float division to a reciprocal plus
/// a Newton refinement rather than to a correctly-rounded divide, so
/// evaluating the same quantity as a division returns rho off by an ulp for
/// some directions and not others -- measured here as a 4-bounce throughput
/// of 0.0624999963 instead of 0.0625 for part of the path set. The
/// probe checks that assert an exact power of two after N bounces (14, 17,
/// 21) are only meaningful because this branch exists; with the division
/// they would have had to be relaxed to a tolerance, which is exactly the
/// kind of quiet weakening this stage is under instructions not to do.
///
/// TWO MATERIALS, AND WHY THE SECOND ONE EXISTS (Stage 1 Task 3). The
/// `s*`-prefixed material is the one every SAMPLING DECISION is made from --
/// the lobe choice and the VNDF's alpha -- while the unprefixed one is what
/// `f` and the density are EVALUATED with. `diffBsdfSample` below passes the
/// same material twice, which is the only configuration any renderer uses and
/// which reproduces the previous single-material body expression for
/// expression (the two `diffBsdfSpecProb` calls receive identical arguments
/// and so return identical bits).
///
/// The split exists for the FINITE-DIFFERENCE REFERENCE, not for rendering.
/// Spec section 6.3 lists sampled directions as NOT differentiated -- detached
/// sampling -- so the derivative this subsystem computes is the derivative of
/// the estimator AT FIXED DIRECTIONS. A finite difference that perturbs
/// roughness or metallic and re-runs the sampler measures something else: it
/// measures that derivative PLUS the movement of the sampled direction, which
/// is the term the adjoint deliberately omits. Freezing the `s*` material at
/// theta_0 while the evaluated material moves to theta_0 +/- h makes the
/// difference quotient measure exactly what the adjoint computes -- and
/// because the frozen material also fixes every EARLIER bounce's direction,
/// the whole path is held still, not just this vertex's draw.
///
/// This is a measurement instrument, not a physically meaningful BSDF: with
/// the two materials different, the returned `weight` is f*cos/p with `f` and
/// `p` from one material and the direction drawn from another, which is an
/// unbiased estimator of nothing in particular. The probe that uses it says
/// so; production code passes one material through `diffBsdfSample`.
void diffBsdfSampleDetached(vec3 N, vec3 V, vec3 baseColor, float roughness, float metallic,
                            float specularWeight, vec3 sBaseColor, float sRoughness,
                            float sMetallic, float sSpecularWeight, vec2 uDir, float uLobe,
                            out vec3 L, out vec3 weight, out float pdf) {
    // The density the LOBE CHOICE is made from -- the sampling material's.
    const float qs =
        diffBsdfSpecProb(N, V, sBaseColor, sRoughness, sMetallic, sSpecularWeight);
    // The evaluated material's own q, which is what decides whether the
    // pure-Lambert fast path below is the right answer for `f` and `pdf`.
    const float q = diffBsdfSpecProb(N, V, baseColor, roughness, metallic, specularWeight);
    const float NdotV = dot(N, V);

    // BOTH must be zero to take the exact fast path. With one material (the
    // only configuration any renderer uses) qs == q and this is the identical
    // `q <= 0` test the single-material body had. With two, taking it on the
    // strength of either one alone would return `baseColor` for a material
    // whose f is not Lambertian, or divide when the closed form was exact.
    if (qs <= 0.0 && q <= 0.0) {
        L = diffCosineHemisphere(N, uDir.x, uDir.y);
        const float NdotL = dot(N, L);
        pdf = NdotL * DIFF_BSDF_INV_PI;
        // dot(N, L) > 0 by construction, so the only way this direction can
        // carry no energy is the viewer being on the far side of the
        // surface -- which a one-sided opaque BSDF answers with zero.
        weight = (NdotV > DIFF_BSDF_MIN_COS) ? (baseColor * (1.0 - metallic)) : vec3(0.0);
        return;
    }

    if (uLobe < qs && NdotV > DIFF_BSDF_MIN_COS) {
        // GGX VNDF (Heitz 2018): sample a visible microfacet normal in the
        // local frame, then reflect the view direction about it. `sRoughness`,
        // not `roughness`: this is a sampling decision.
        const float alpha = sRoughness * sRoughness;
        vec3 T, B;
        ggxBuildBasis(N, T, B);
        const vec3 Vloc = normalize(vec3(dot(V, T), dot(V, B), NdotV));
        const vec3 Hloc = sampleGGXVNDF(Vloc, alpha, alpha, uDir);
        const vec3 H = normalize(Hloc.x * T + Hloc.y * B + Hloc.z * N);
        L = normalize(reflect(-V, H));
    } else {
        L = diffCosineHemisphere(N, uDir.x, uDir.y);
    }

    vec3 f;
    diffBsdfEval(N, V, L, baseColor, roughness, metallic, specularWeight, f, pdf);

    const float NdotL = dot(N, L);
    if (pdf <= 0.0 || NdotL <= 0.0) {
        // The VNDF's below-horizon tail, or a grazing configuration the
        // specular math refuses. The BRDF is zero at such a direction, so
        // the sample carries no energy: report a zero weight and a zero
        // density rather than dividing by a density the direction was not
        // drawn from (which is what "snap to the mirror lobe" silently
        // does). See the header note -- L stays as drawn.
        pdf = 0.0;
        weight = vec3(0.0);
        return;
    }

    weight = f * NdotL / pdf;
}

/// The ONE-MATERIAL entry point every renderer uses: samples and evaluates
/// with the same material. Kept as the whole of the previous signature so no
/// call site changed, and defined as a delegation rather than as a second
/// body so there is exactly one implementation of the sampler -- the same
/// rule this file's header states about the microfacet terms.
void diffBsdfSample(vec3 N, vec3 V, vec3 baseColor, float roughness, float metallic,
                    float specularWeight, vec2 uDir, float uLobe, out vec3 L, out vec3 weight,
                    out float pdf) {
    diffBsdfSampleDetached(N, V, baseColor, roughness, metallic, specularWeight, baseColor,
                           roughness, metallic, specularWeight, uDir, uLobe, L, weight, pdf);
}

#endif  // OHAO_DIFF_BSDF_GLSL
