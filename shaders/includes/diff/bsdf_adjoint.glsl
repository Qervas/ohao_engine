#ifndef OHAO_DIFF_BSDF_ADJOINT_GLSL
#define OHAO_DIFF_BSDF_ADJOINT_GLSL

// ===========================================================================
// THE FIRST ADJOINT -- d(film)/d(albedo) at ONE vertex
// ===========================================================================
//
// This file is the derivative half of shaders/includes/diff/bsdf.glsl. It is
// included by shaders/includes/diff/traverse.glsl AFTER `DiffVertex` is
// declared (it takes one) and after the bindings and the Push block (it reads
// `pc.albedo`), and it is compiled into BOTH instantiations of the traversal.
// Only the REPLAY instantiation calls it; the forward one links it and never
// reaches it, which is the same shape the film write has in reverse.
//
// It is a separate file rather than lines inside a hook because a `.comp`
// instantiation may define exactly two functions -- `diffVertexHook` and
// `main` -- and `diff_gpu_probe.cpp`'s `checkTraverseInstantiationTie()`
// refuses to run the probe if either grows a third. That rule exists to stop
// a second traversal hiding in an includer; the consequence here is that
// every helper a hook needs lives in a shared include, which is where a
// derivative belongs anyway: one definition, compiled identically into both
// kernels.
//
// ---------------------------------------------------------------------------
// THE OBJECTIVE THIS IS THE DERIVATIVE OF, stated before anything else
// ---------------------------------------------------------------------------
//
// A gradient is meaningless without naming the scalar it is the gradient of.
// Everything below is the derivative of
//
//     J(theta) = SUM over PIXELS p, SUM over CHANNELS c of film[p][c]
//
// where `film` is exactly what the FORWARD instantiation's hook accumulates
// (shaders/diff/wf_scatter.comp): the MIS-combined direct lighting at every
// surface vertex, carried by the path throughput on arrival, truncated at the
// loop's bounce count, with no emissive and no escape term. J is a scalar, so
// dJ/d(albedo) is a scalar, and it is what this file scatters into ONE float
// of the gradient arena.
//
// The finite-difference gate that measures it (diff_gpu_probe.cpp) forms the
// SAME J from the same film buffer, at albedo +/- h under common random
// numbers. The two sides therefore agree about what is being differentiated
// by construction, and disagree about nothing else.
//
// ---------------------------------------------------------------------------
// WHY THE ADJOINT NEEDS NO STORAGE, AND WHAT `v.adjoint` IS
// ---------------------------------------------------------------------------
//
// The PRB recursion propagates an adjoint dL from vertex to vertex:
//
//     dL_0 = dJ/d(L_r at the first vertex) = 1      (J is a plain SUM of film)
//     dL_{b+1} = dL_b * bsdfWeight_b                (DiffVertex's propagate line)
//
// The path's own throughput obeys the IDENTICAL recursion:
//
//     T_0 = 1,   T_{b+1} = T_b * bsdfWeight_b
//
// -- `bsdfWeight` is bit-exactly what `psSetThroughput` multiplies by. So for
// THIS objective the adjoint and the throughput are the same sequence, and
// the wavefront already carries it, in path state, under the name
// `throughput`. That is why Stage 1 Task 2 adds no adjoint field to path
// state and why the hook contract's ban on writing path state costs nothing
// here: `v.throughput` (the ARRIVAL throughput, before this vertex's decay)
// IS dL_b.
//
// This is an identity for a sum-of-film objective, NOT a general fact. The
// moment the objective acquires a per-pixel weight -- an L2 image loss, say,
// whose adjoint is 2*(film - target)[p] -- dL_0 stops being 1, the two
// sequences separate at the first vertex, and the adjoint must be carried
// (a path-state field, seeded per pixel). Nothing here silently generalises:
// the hook seeds `v.adjoint` from `v.throughput` in one visible line, and a
// later task changes that line.
//
// `v.adjoint` is still written back by the hook, propagated by the line
// above, because that is what `DiffVertex`'s signature says the field is for.
// It is not read again in this task (the traversal discards `v` at the
// return), and this comment says so rather than leaving a reader to assume it
// feeds the next bounce -- it does not; the next bounce's dispatch re-reads
// throughput out of path state.
//
// ---------------------------------------------------------------------------
// THE SCATTER LINE, AND THE TERM `DiffVertex`'s RECURSION DOES NOT CONTAIN
// ---------------------------------------------------------------------------
//
// `DiffVertex`'s comment (traverse.glsl) states the recursion as
//
//     dL/dtheta += dL * SUM_s [ w_s * (df_s/dtheta) * cos_s * L_s * V_s / p_s ]
//     dL_next    = dL * bsdfWeight
//
// and that scatter line is implemented VERBATIM below, as
// `diffVertexDirectAlbedoScatter`. It is exactly the derivative of the DIRECT
// LIGHTING formed at this vertex, carried by the adjoint.
//
// IT IS NOT THE WHOLE DERIVATIVE OF J once the loop runs more than one
// bounce, and this was MEASURED, not reasoned about after the fact: the
// finite-difference gate rejects the direct term alone at two bounces and
// accepts direct + the term below (diff_gpu_probe.cpp's gradient checks
// report both numbers). The derivation:
//
//     J = SUM_b T_b * Lr_b          (the forward hook, summed over bounces)
//     dJ/dtheta = SUM_b [ (dT_b/dtheta) * Lr_b  +  T_b * (dLr_b/dtheta) ]
//                        \__ the term below __/    \__ the scatter line __/
//
// The scatter line is the second half only. The first half exists because the
// THROUGHPUT is itself a function of theta -- every earlier vertex multiplied
// it by a BSDF weight that depends on the albedo -- and it does not vanish:
// with a pure Lambertian surface T_b = albedo^b exactly, so
// dT_b/d(albedo) = b * albedo^(b-1) and the omitted term is the same order of
// magnitude as the retained one. At two bounces, dropping it halves the
// second bounce's contribution.
//
// A reverse-mode formulation would fold this into the scatter line by using
// the FULL incident radiance along the continuation direction (the spec's
// L_i) rather than the environment radiance along it; this integrator's
// vertex does not have that -- it is the future of the path -- which is
// precisely why PRB needs either a stored primal or a second walk. What is
// available here is the closed form for dT_b/dtheta, and it is what
// `diffVertexThroughputAlbedoTerm` uses.
//
// ---------------------------------------------------------------------------
// THE ONE MATERIAL CONFIGURATION THIS FILE IS EXACT FOR
// ---------------------------------------------------------------------------
//
// Everything below is written for `metallic == 0` and `specularWeight == 0`
// -- the PURE LAMBERTIAN configuration, which is the whole of the material
// model Stage 1 Task 2 differentiates. Two things stop holding outside it and
// they fail in opposite directions, so neither is a rounding matter:
//
//   * `metallic > 0` makes f depend on the base colour through
//     F0 = mix(0.04, baseColor, metallic) in BOTH the specular lobe and the
//     lobe-selection probability q. `diffAlbedoDerivOfFGrey` returns only the
//     DIFFUSE lobe's (1-metallic)/pi and would understate df/d(albedo); worse,
//     q would depend on albedo, so the SAMPLED DIRECTION would move under the
//     finite-difference perturbation and common random numbers would stop
//     holding at all.
//   * `specularWeight > 0` (at metallic 0) leaves df/d(albedo) correct -- the
//     dielectric specular lobe's F0 is the constant 0.04 -- but breaks
//     `diffVertexThroughputAlbedoTerm`, whose closed form
//     dT_b/d(albedo) = b*T_b/albedo rests on bsdf.glsl's pure-Lambert fast
//     path returning a per-bounce weight of EXACTLY `albedo`.
//
// This is not left as a comment to be obeyed. The probe entry point that
// drives a gradient run (`GpuProbeContext::runWavefrontGradientProbe`)
// REFUSES to dispatch unless both are zero, and says why. Task 3 replaces the
// bodies below; the guard is what stops that being optional.

/// d(f)/d(albedo) for the GREY base-colour scalar the wavefront pushes, at
/// the direction `L`, WITHOUT the cosine. `f` here is the same f
/// `diffBsdfEval` returns, so this mirrors that function's rejection band
/// exactly rather than approximating it: f is identically zero below
/// DIFF_BSDF_MIN_COS on either side, so the derivative is identically zero
/// there too. Using `max(dot(N,L), 0)` instead would report a small nonzero
/// derivative in the band (0, 1e-4] where the shader's f is pinned at 0 --
/// tiny, but a systematic disagreement with the film the gate differentiates,
/// which is exactly what a gate is for.
///
/// Returns the DIFFUSE lobe's derivative only. See the header: that is the
/// whole of f's albedo dependence at metallic == 0, and the caller is
/// guaranteed metallic == 0 by the host-side refusal named there.
float diffAlbedoDerivOfFGrey(vec3 N, vec3 V, vec3 L, float metallic) {
    const float NdotL = dot(N, L);
    const float NdotV = dot(N, V);
    if (NdotL <= DIFF_BSDF_MIN_COS || NdotV <= DIFF_BSDF_MIN_COS) return 0.0;
    return (1.0 - metallic) * DIFF_BSDF_INV_PI;
}

/// `DiffVertex`'s scatter line, verbatim, for theta = the pushed grey albedo.
///
///     dL * SUM_s [ w_s * (df_s/dtheta) * cos_s * L_s * V_s / p_s ]
///
/// THE cos_B ASYMMETRY, WHICH IS WHERE THIS GOES WRONG IF IT GOES WRONG.
/// The two strategies do not carry their cosine in the same place, and the
/// two lines below therefore look identical for OPPOSITE reasons:
///
///   * s = E. The traversal formed this strategy's integrand as
///     `fEnv * nDotLEnv`, where `fEnv` came from `diffBsdfEval` and carries
///     NO cosine. So `cos_E` is a genuine extra factor of the scatter line
///     and is multiplied in here, as `max(dot(normal, envDir), 0)` -- the
///     same expression the traversal used.
///   * s = B. `v.f` is f*cos ALREADY (traverse.glsl derives it as
///     `weight * pdf`), so the quantity this term needs is d(v.f)/dtheta,
///     not df/dtheta. Differentiating f*cos with respect to the albedo gives
///     (df/d(albedo)) * cos -- the cosine is a constant of the
///     differentiation, not a factor the scatter line supplies -- so the
///     cosine appears here too, and multiplying it in a second time (or
///     leaving it out on the grounds that "it is already in f") is the
///     silent factor-of-cos error this asymmetry invites. The cosine used is
///     `max(dot(normal, bsdfDir), 0)`: at `bsdfDir`, NOT at `wi`. They differ
///     when the GGX VNDF drew below the horizon, and `bsdfDir` is the
///     direction the BSDF was evaluated at.
///
/// Both divisions are guarded on a strictly positive density, matching
/// nee.glsl's `diffMisTerm`: where the density is zero the traversal's own
/// contribution is zero, so the derivative of it is zero and not a NaN.
vec3 diffVertexDirectAlbedoScatter(in DiffVertex v, vec3 dL) {
    if (!v.hit) return vec3(0.0);

    float sum = 0.0;

    // s = E -- next event. cos_E is NOT inside f_E.
    if (v.envPdf > 0.0) {
        const float cosE = max(dot(v.normal, v.envDir), 0.0);
        const float dfE = diffAlbedoDerivOfFGrey(v.normal, v.wo, v.envDir, v.metallic);
        sum += v.wEnv * dfE * cosE * v.envRadiance * v.visEnv / v.envPdf;
    }

    // s = B -- BSDF sampling. cos_B IS inside v.f, so it is inside d(v.f).
    if (v.pdf > 0.0) {
        const float cosB = max(dot(v.normal, v.bsdfDir), 0.0);
        const float dfB = diffAlbedoDerivOfFGrey(v.normal, v.wo, v.bsdfDir, v.metallic) * cosB;
        sum += v.wBsdf * dfB * v.bsdfRadiance * v.visBsdf / v.pdf;
    }

    // The albedo is ONE grey scalar feeding all three channels
    // (`vtx.baseColor = vec3(pc.albedo)`), so the derivative is the same
    // number in each; the per-channel structure comes entirely from `dL`.
    return dL * sum;
}

/// The term the recursion in `DiffVertex` does not contain:
/// (dT_b/d(albedo)) * Lr_b. See this file's header for the derivation and
/// for the measurement that established it is needed.
///
/// T_b is `v.throughput`, the throughput on ARRIVAL at this vertex, and in
/// the pure-Lambertian configuration this file is exact for it is EXACTLY
/// albedo^bounce -- bsdf.glsl's fast path returns `baseColor` with no
/// division at all, which is the same exactness diff_gpu_probe.cpp's checks
/// 14/17/35 assert bit-for-bit. Hence
///
///     dT_b/d(albedo) = b * albedo^(b-1) = (b / albedo) * T_b
///
/// written in the T_b form so that the value used is the throughput the
/// traversal actually carried rather than a power recomputed here.
///
/// `Lr` is the MIS-combined REFLECTED direct radiance at this vertex. It is
/// used here and it is emphatically NOT being substituted into the scatter
/// line: this term multiplies it by the derivative of the THROUGHPUT, which
/// is the one place a reflected radiance belongs. (The scatter line above
/// never mentions it.)
///
/// Bounce 0 contributes nothing -- T_0 = 1 for every albedo -- and the
/// `albedo <= 0` guard is a division guard, not a physical case: at albedo 0
/// the throughput is zero from bounce 1 on and so is this term's limit.
vec3 diffVertexThroughputAlbedoTerm(in DiffVertex v, float albedo) {
    if (!v.hit || v.bounce == 0u || albedo <= 0.0) return vec3(0.0);
    return (float(v.bounce) / albedo) * v.throughput * v.Lr;
}

#endif  // OHAO_DIFF_BSDF_ADJOINT_GLSL
