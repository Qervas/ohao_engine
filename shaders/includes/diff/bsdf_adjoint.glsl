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
// THE TWO TERMS, AND WHERE THE RECURSION THEY IMPLEMENT IS DECLARED
// ---------------------------------------------------------------------------
//
// `DiffVertex`'s comment in traverse.glsl carries the recursion and the
// derivation of BOTH its terms; it is the declaration this file is written
// against and the thing to read first. THIS FILE IS THE IMPLEMENTATION, not a
// second statement of the maths, and where the two could drift the comment
// there wins. In brief, for reading the two functions below:
//
//     dL/dtheta += (dT_b/dtheta) * Lr                 <- ...ThroughputAlbedoTerm
//               +  dL * SUM_s [ w_s * (df_s/dtheta)
//                               * cos_s * L_s * V_s / p_s ]  <- ...DirectAlbedoScatter
//     dL_next    =  dL * bsdfWeight                   <- the hook's third line
//
// -- because the film is J = SUM_b T_b * Lr_b and BOTH factors of each summand
// depend on theta.
//
// THE SPLIT WAS MEASURED, not reasoned about after the fact. For one task this
// file implemented the direct term alone, matching what `DiffVertex`'s comment
// then (wrongly) called "the line every consumer must use"; the
// finite-difference gate accepted it at one bounce and rejected it at two and
// three, by roughly 500x its own error bound. With a pure Lambertian surface
// T_b = albedo^b exactly, so dropping the throughput term HALVES the second
// bounce's contribution -- it is not a correction, it is half the answer.
//
// A reverse-mode formulation would fold that term into the direct one by using
// the FULL incident radiance along the continuation direction (the spec's L_i)
// rather than the environment radiance along it; this integrator's vertex does
// not have that -- it is the future of the path -- which is precisely why PRB
// needs either a stored primal or a second walk. What IS available here is the
// closed form for dT_b/dtheta, and it is what `diffVertexThroughputAlbedoTerm`
// uses.
//
// A THIRD TERM, (dw_s/dtheta), is identically zero here and traverse.glsl says
// why: at metallic 0 and specularWeight 0 neither `pdf` nor `pdfEnvMap`
// mentions the base colour, so the MIS weights are constants of this
// differentiation. Task 3 adds it when they stop being constants.
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

/// ---------------------------------------------------------------------------
/// TWO DERIVATIVES, BECAUSE THE TWO STRATEGIES' `f` CAME OUT OF DIFFERENT
/// FUNCTIONS WITH DIFFERENT REJECTION BANDS
/// ---------------------------------------------------------------------------
///
/// A derivative must mirror the rejection band of the function that produced
/// the value it differentiates, not the band of some other function that
/// computes the same physics. This file gets two of them because the
/// traversal used two:
///
///   * The NEE strategy's `fEnv` came from `diffBsdfEval`, which rejects on
///     `NdotL <= 1e-4 || NdotV <= 1e-4` and pins f to zero in that band.
///   * The BSDF strategy's `v.f` is `weight * pdf` from `diffBsdfSample`. In
///     the pure-Lambert configuration this file is exact for, the sampler
///     takes its `q <= 0` fast path, which sets `pdf = NdotL * INV_PI` and
///     `weight = baseColor * (1 - metallic)` with NO NdotL rejection at all --
///     only an `NdotV > 1e-4` test on the weight.
///
/// ONE function guarded both branches for one task, and the NdotL half of its
/// band was wrong for the BSDF branch: it returned 0 in 0 < NdotL <= 1e-4,
/// where the film's own B-strategy contribution is FULL MAGNITUDE -- the
/// density cancels out of the estimator (nee.glsl's `unweighted` is
/// f*cos*L*V/pdf = weight*L*V), so a vanishing pdf does not vanish from the
/// film. Cosine-hemisphere sampling gives NdotL ~ sqrt(1 - u1), so that band
/// needs u1 >= 1 - 1e-8 and was never observed at this probe's sample count;
/// it was a genuine film/derivative disagreement all the same, and a gate
/// exists to be right about the things it cannot yet see.

/// d(f)/d(albedo) for the GREY base-colour scalar the wavefront pushes, at
/// the direction `L`, WITHOUT the cosine -- FOR THE NEE STRATEGY ONLY.
///
/// `f` here is the same f `diffBsdfEval` returns, so this mirrors THAT
/// function's rejection band exactly rather than approximating it: f is
/// identically zero below DIFF_BSDF_MIN_COS on either side, so the derivative
/// is identically zero there too. Using `max(dot(N,L), 0)` instead would
/// report a small nonzero derivative in the band (0, 1e-4] where the shader's
/// f is pinned at 0 -- tiny, but a systematic disagreement with the film the
/// gate differentiates, which is exactly what a gate is for. Applying this
/// band to the BSDF strategy is the SAME disagreement in the other direction,
/// which is what `diffAlbedoDerivOfFCosAtBsdfDir` below exists to avoid.
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

/// d(v.f)/d(albedo) -- the derivative of f*cos AT `v.bsdfDir`, for the BSDF
/// strategy. This is deliberately NOT `diffAlbedoDerivOfFGrey(...) * cos`: it
/// is the derivative of the quantity the traversal ACTUALLY STORED, with that
/// quantity's own guard.
///
/// `v.f` is `weight * pdf`, and in the configuration this file is exact for
/// (metallic == 0 and specularWeight == 0, so `diffBsdfSpecProb` returns 0 and
/// `diffBsdfSample` takes its `q <= 0` fast path)
///
///     weight = baseColor * (1 - metallic)   when NdotV > DIFF_BSDF_MIN_COS,
///              vec3(0)                      otherwise
///     pdf    = NdotL * INV_PI               with NdotL > 0 by construction
///
/// so d(v.f)/d(albedo) is exactly `(1 - metallic) * v.pdf` inside the weight's
/// NdotV band and exactly 0 outside it. THE DENSITY IS USED DIRECTLY rather
/// than recomputing NdotL/pi here, for the same reason the traversal forms `f`
/// as `weight * pdf`: it is the number the forward pass used, so no
/// re-derivation can drift from it by an ulp -- and it makes the cosine
/// impossible to double-count, because it is never written down twice.
///
/// `v.pdf` is the MIXTURE density in general, not a lobe-conditional one, so
/// reading it as the diffuse lobe's density is sound ONLY because this
/// configuration has a single lobe (q == 0 identically). Task 3, which lifts
/// that restriction, has to split the density here as well as extend the
/// numerator.
float diffAlbedoDerivOfFCosAtBsdfDir(in DiffVertex v) {
    if (dot(v.normal, v.wo) <= DIFF_BSDF_MIN_COS) return 0.0;
    return (1.0 - v.metallic) * v.pdf;
}

/// The DIRECT term of `DiffVertex`'s recursion, verbatim, for theta = the
/// pushed grey albedo. It is one of the recursion's TWO scatter terms --
/// `diffVertexThroughputAlbedoTerm` is the other, and the hook adds them.
///
///     dL * SUM_s [ w_s * (df_s/dtheta) * cos_s * L_s * V_s / p_s ]
///
/// THE cos_B ASYMMETRY, WHICH IS WHERE THIS GOES WRONG IF IT GOES WRONG.
/// The two strategies do not carry their cosine in the same place:
///
///   * s = E. The traversal formed this strategy's integrand as
///     `fEnv * nDotLEnv`, where `fEnv` came from `diffBsdfEval` and carries
///     NO cosine. So `cos_E` is a genuine extra factor of the scatter line
///     and is multiplied in here, as `max(dot(normal, envDir), 0)` -- the
///     same expression the traversal used -- and the derivative beside it is
///     `diffAlbedoDerivOfFGrey`, which mirrors `diffBsdfEval`'s band because
///     that is the function `fEnv` came out of.
///   * s = B. `v.f` is f*cos ALREADY (traverse.glsl derives it as
///     `weight * pdf`), so the quantity this term needs is d(v.f)/dtheta and
///     the cosine is a CONSTANT of that differentiation, not a factor the
///     scatter line supplies. Multiplying a cosine in a second time -- or
///     leaving it out on the grounds that "it is already in f" -- is the
///     silent factor-of-cos error this asymmetry invites, so no cosine is
///     written here at all: `diffAlbedoDerivOfFCosAtBsdfDir` returns
///     d(v.f)/d(albedo) whole, out of `v.pdf`, which IS NdotL/pi at
///     `bsdfDir`. That is also where the derivative has to be taken -- at
///     `bsdfDir`, NOT at `wi`; they differ when the GGX VNDF drew below the
///     horizon, and `bsdfDir` is the direction the BSDF was evaluated at.
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

    // s = B -- BSDF sampling. cos_B IS inside v.f, so it is inside d(v.f),
    // which is returned whole rather than rebuilt from a cosine this file
    // would otherwise have to recompute.
    if (v.pdf > 0.0) {
        const float dfB = diffAlbedoDerivOfFCosAtBsdfDir(v);
        sum += v.wBsdf * dfB * v.bsdfRadiance * v.visBsdf / v.pdf;
    }

    // The albedo is ONE grey scalar feeding all three channels
    // (`vtx.baseColor = vec3(pc.albedo)`), so the derivative is the same
    // number in each; the per-channel structure comes entirely from `dL`.
    return dL * sum;
}

/// The recursion's OTHER scatter term: (dT_b/d(albedo)) * Lr_b, the one that
/// exists because the throughput arriving at this vertex is itself a function
/// of theta. `DiffVertex`'s comment carries the derivation and the measurement
/// that established it is needed; this file's header summarises both. It went
/// missing from every statement of the recursion in this repository for one
/// task, which is why both statements now name it explicitly.
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
