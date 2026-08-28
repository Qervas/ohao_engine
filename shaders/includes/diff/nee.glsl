#ifndef OHAO_DIFF_NEE_GLSL
#define OHAO_DIFF_NEE_GLSL

// Next-event estimation and multiple importance sampling for the wavefront
// integrator, as ONE parameterised implementation.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS AT ALL: the duplication it refuses to reproduce
// ---------------------------------------------------------------------------
//
// shaders/rt/pt_raygen.rgen carries the same estimator FOUR times: an
// analytic-light next-event block at :584 and again at :892, and an
// environment MIS block at :707 and again at :1015. Each pair is the same
// computation written out twice, differing only in which accumulator the
// result is added to (`specContrib` vs `diffContrib`, both also added to
// `radiance`). This project has been bitten three separate times by
// duplicated GPU code -- camera_ray.glsl, loadSpv, and one barrier that
// ended up with three hand-maintained copies -- so the port here is a single
// function whose CALLER chooses the accumulation target. The accumulation
// target was the only axis along which those blocks ever differed; it is a
// caller's business, and it is the only thing left outside this file.
//
// The MIS heuristics themselves are not re-implemented either:
// shaders/includes/rt/mis.glsl already provides misBalanceHeuristic and
// misPowerHeuristic, and this file calls the former rather than writing a
// third variant of the same three lines.
//
// ---------------------------------------------------------------------------
// THE ESTIMATOR, stated as a contract
// ---------------------------------------------------------------------------
//
// Two strategies estimate ONE integral, the direct-lighting integral at a
// shading point:
//
//     I = integral over the sphere of  f(N,V,w) * max(0, N.w) * L(w) * V(w) dw
//
// Strategy E ("next event"): draw w proportional to the environment's
// sin(theta)-weighted luminance (shaders/includes/rt/env_sampling.glsl's
// sampleEnvMap), density p_E(w).
// Strategy B ("BSDF sampling"): draw w from the BSDF's own mixture density
// (shaders/includes/diff/bsdf.glsl's diffBsdfSample), density p_B(w).
//
// Each strategy's own unbiased estimator of I from one sample is
// f*cos*L*V / p_own. MIS combines them with weights that sum to 1 AT EVERY
// DIRECTION -- the balance heuristic's defining property,
// p_A/(p_A+p_B) + p_B/(p_A+p_B) = 1 -- so that
//
//     E[ w_E(w_e) * est_E(w_e) + w_B(w_b) * est_B(w_b) ] = I
//
// exactly, for any weighting that partitions unity pointwise. That per-sample
// partition is the thing diff_gpu_probe.cpp asserts directly (it is exact and
// cheap, and it catches an entire class of weighting bug that a convergence
// test only ever sees as noise), which is why diffMisTerm returns BOTH halves
// of the partition and not just the caller's own half.
//
// ---------------------------------------------------------------------------
// RECOVERING RADIANCE FROM A DENSITY, and why envIntegral stops being dead
// ---------------------------------------------------------------------------
//
// The wavefront stage binds the environment's two CDF arrays and nothing
// else: there is no environment radiance image in this pipeline's descriptor
// set. It does not need one. The CDF is built (ohao/render/rt/env_cdf.cpp)
// from luminance weighted by sin(theta) and normalised by its own total, so
// the solid-angle density sampleEnvMap returns is
//
//     p(w) = L_grey(x,y) * sin(theta) / integral
//            / [ (2*pi/W)(pi/H) sin(theta) ]
//          = L_grey(x,y) * W * H / (integral * 2*pi^2),
//
// the sine cancelling exactly (which is check 25's subject). Inverting,
//
//     L_grey(w) = p(w) * integral * 2*pi^2 / (W*H),
//
// which is diffEnvRadianceFromPdf below. `integral` is EnvCDF::integral(),
// carried to the shader as ScatterPush::envIntegral -- the field
// env_sampling.glsl's own header notes is accepted and never read, and which
// consequently nothing in this repository verified had arrived intact. It is
// load-bearing here: it is the entire scale of every NEE contribution, and a
// wrong value would rescale all three estimators together, which no
// agreement-between-estimators check could ever see. diff_gpu_probe.cpp
// therefore asserts the recovered radiance against the environment image it
// built, texel by texel, rather than leaving the scale to a relative check.
//
// ACHROMATIC, on purpose. What comes back is the CDF's grey channel
// (0.2126 R + 0.7152 G + 0.0722 B), not RGB: the CDF is the only environment
// data bound. For a grey environment -- which is what the probe builds, and
// what every check here measures -- that is the exact radiance. For a
// coloured one it is the correct luminance and the wrong chroma, and fixing
// that means binding the environment image itself, which is plumbing with no
// consumer in this stage yet. Nothing here silently pretends otherwise.

#include "rt/mis.glsl"

// 2*pi^2. Spelled out rather than reusing env_sampling.glsl's OHAO_PI /
// OHAO_TWOPI so that this header can be included BEFORE the environment
// bindings it would otherwise have to follow (env_sampling.glsl must come
// after `envMarg`/`envCond` are declared; this file must not inherit that
// constraint, and redeclaring a `const float` GLSL already has is an error,
// not a shadow).
const float DIFF_NEE_TWO_PI_SQUARED = 19.7392088021787172;

/// Grey radiance of the environment at a direction whose sampling density
/// (solid-angle measure) is `pdf`. See the header derivation. Returns 0 for
/// an unconfigured environment (W or H zero), which is the same "no
/// environment" sentinel wf_scatter.comp writes a zero pdf for.
float diffEnvRadianceFromPdf(float pdf, uint W, uint H, float envIntegral) {
    if (W == 0u || H == 0u) return 0.0;
    return pdf * envIntegral * DIFF_NEE_TWO_PI_SQUARED / (float(W) * float(H));
}

/// One strategy's contribution to the direct-lighting integral, plus BOTH
/// halves of the MIS partition at the direction that strategy drew.
struct DiffMisTerm {
    /// f * cos * L * V / p_own -- this strategy's own unbiased estimator of
    /// the direct-lighting integral, with NO MIS weight applied. The weight
    /// is returned separately so a caller (and a probe) can form the
    /// single-strategy estimator, the MIS combination, or both, from the
    /// same sample.
    vec3 unweighted;
    /// MIS weight for the strategy that DREW this sample.
    float wOwn;
    /// MIS weight the OTHER strategy would carry at the SAME direction.
    /// wOwn + wOther == 1 for every sample with p_own + p_other >= the
    /// heuristic's 1e-6 floor; that identity is asserted per sample by
    /// diff_gpu_probe.cpp.
    float wOther;
};

/// The ONE implementation both strategies go through.
///
/// `fCosine`  = f(N,V,w) * max(0, N.w) at the drawn direction.
/// `radiance` = L(w), e.g. from diffEnvRadianceFromPdf.
/// `visibility` = the shadow ray's answer, 1 unoccluded / 0 occluded. Passed
///              in rather than traced here because the acceleration
///              structure is the calling stage's binding, exactly as
///              env_sampling.glsl leaves `envMarg`/`envCond` to its caller.
/// `pdfOwn`   = the density the direction was drawn from.
/// `pdfOther` = the OTHER strategy's density at that same direction.
///
/// A non-positive `pdfOwn` yields a zero contribution rather than a
/// division: the GGX VNDF's below-horizon tail returns exactly that, and a
/// direction below the horizon carries no energy anyway.
DiffMisTerm diffMisTerm(vec3 fCosine, vec3 radiance, float visibility, float pdfOwn,
                        float pdfOther) {
    DiffMisTerm term;
    term.wOwn = misBalanceHeuristic(pdfOwn, pdfOther);
    term.wOther = misBalanceHeuristic(pdfOther, pdfOwn);
    term.unweighted =
        (pdfOwn > 0.0) ? (fCosine * radiance * visibility / pdfOwn) : vec3(0.0);
    return term;
}

#endif  // OHAO_DIFF_NEE_GLSL
