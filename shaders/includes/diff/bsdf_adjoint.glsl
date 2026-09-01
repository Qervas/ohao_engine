#ifndef OHAO_DIFF_BSDF_ADJOINT_GLSL
#define OHAO_DIFF_BSDF_ADJOINT_GLSL

// ===========================================================================
// THE ADJOINTS -- d(film)/d(theta) at ONE vertex
// ===========================================================================
//
// THIS FILE HAS FOUR PARTS, and the split is by PARAMETER, not by
// convenience. The first is Stage 1 Task 2's d(film)/d(albedo), which is
// exact only for the pure Lambertian configuration and uses a closed form for
// the throughput term. The second, from the banner "STAGE 1 TASK 3" onward,
// is d(film)/d(roughness) and d(film)/d(metallic) through the microfacet
// model, which have no closed form for the throughput and which move the
// densities and therefore the MIS weights. The third, from "STAGE 1 TASK 4"
// onward, is d(film)/d(emission) -- the simplest of the three, because
// emission is ADDED to the film rather than sampled from or multiplied
// through a density, so none of Task 3's machinery (a detached instrument, a
// carried tangent, a moving MIS weight) is needed. The fourth, from "STAGE 1
// TASK 5" onward, is d(film)/d(an EMISSION TEXTURE) -- the same physics as
// the third, distributed over four texels by a bilinear reconstruction, and
// the FIRST parameter in this subsystem that is not a scalar. `pc.diffParam`
// selects among all four at the hook, and the constants that name it
// (DIFF_PARAM_*) are declared in the second part. Everything down to the
// "STAGE 1 TASK 3" banner is about the albedo alone and says so wherever it
// makes a claim.
//
// THE FOURTH PART ALSO CARRIES A FORWARD FUNCTION, WHICH THE OTHER THREE DO
// NOT, and the exception is deliberate: a texture read's adjoint must use the
// weights of the reconstruction the forward read actually used, so the
// reconstruction and its derivative live side by side here rather than in two
// files that could drift into being two different functions. That is the ONE
// place in Stage 1 where forward/adjoint sharing is required rather than
// forbidden, and the "STAGE 1 TASK 5" banner argues it at length.
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
// loop's bounce count, PLUS -- since Stage 1 Task 4 -- a uniform self-emission
// term at every hit vertex, with no escape term still. J is a scalar, so
// dJ/d(theta) is a scalar for every theta this file differentiates, and it is
// what this file scatters into ONE float of the gradient arena.
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
// A THIRD TERM, (dw_s/dtheta), is identically zero FOR THE ALBEDO and
// traverse.glsl says why: at metallic 0 and specularWeight 0 neither `pdf` nor
// `pdfEnvMap` mentions the base colour, so the MIS weights are constants of
// this differentiation. It is NOT zero for roughness or metallic -- both move
// `pdf` -- and Stage 1 Task 3 writes it out in the second half of this file,
// along with a FOURTH term the albedo also did not have: the BSDF strategy's
// estimator is f*cos*L*V/p_B and p_B moves too, so the quotient has to be
// differentiated and not only its numerator.
//
// ---------------------------------------------------------------------------
// THE ONE MATERIAL CONFIGURATION THE ALBEDO HALF IS EXACT FOR
// ---------------------------------------------------------------------------
//
// Everything down to the Stage 1 Task 3 banner is written for `metallic == 0`
// and `specularWeight == 0`
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
// REFUSES to dispatch unless both are zero WHEN THE PARAMETER IS THE BASE
// COLOUR, and says why. Stage 1 Task 3 did not relax that guard: it added a
// DIFFERENT set of preconditions for its own two parameters (a specular lobe
// must exist, the roughness must clear the 0.01 floor, a metallic run must sit
// strictly inside the [0,1] clamp), because a different derivative needs
// different things to be true. Same function, three parameters, three
// refusals.

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
/// configuration has a single lobe (q == 0 identically), which the host-side
/// refusal named in the header enforces. Stage 1 Task 3 did NOT lift that
/// restriction here: rather than making this function handle a mixture, it
/// added `diffBsdfEvalDeriv` below, which differentiates `diffBsdfEval`'s f
/// and its mixture density from scratch and mirrors that function's bands
/// statement for statement. This one stays what it is -- the albedo
/// derivative of the pure-Lambert fast path -- and the two never have to
/// agree about a case neither of them handles.
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

// ===========================================================================
// STAGE 1 TASK 3 -- df/d(roughness) AND df/d(metallic) THROUGH THE MICROFACET
// ===========================================================================
//
// Everything from here down is the GGX half of this file. It is the
// derivative of the SAME objective the albedo half is the derivative of
// (J = the sum of every float of the film), and it is written against the
// SAME declaration -- `DiffVertex` in traverse.glsl, which remains the
// authority: where this file and that comment could drift, that comment wins.
//
// ---------------------------------------------------------------------------
// WHAT IS DIFFERENTIATED, AND WHAT IS DELIBERATELY NOT
// ---------------------------------------------------------------------------
//
// Spec section 6.3 lists SAMPLED DIRECTIONS as not differentiated (detached
// sampling) and MIS WEIGHTS as differentiated. Taking that literally:
//
//   * Every direction in the estimator -- `v.bsdfDir`, `v.envDir`, and every
//     earlier bounce's direction, which is what put this vertex where it is
//     -- is held FIXED. Nothing below contains a derivative of a direction.
//   * Everything the estimator EVALUATES at those fixed directions is
//     differentiated: `f`, the mixture density `pdf`, the partner density
//     `pdfBsdfAtEnvDir`, and through those two densities the balance-heuristic
//     MIS weights `wEnv` and `wBsdf`.
//
// The film is then a plain arithmetic function of theta at frozen directions,
// and what follows is its exact derivative. THAT IS ALSO WHAT THE
// FINITE-DIFFERENCE REFERENCE MEASURES: `bsdf.glsl`'s `diffBsdfSampleDetached`
// lets the probe hold the sampling material at theta_0 while the evaluated
// material moves to theta_0 +/- h, so the two sides differentiate the same
// function rather than one of them also differentiating the sampler. The
// difference between that reference and a NAIVE common-random-number
// difference is the detached-sampling bias, which the probe measures and
// reports as a finding.
//
// THE (dw_s/dtheta) TERM IS NOT ZERO HERE, and traverse.glsl named this task
// as its owner. At metallic 0 / specularWeight 0 neither density mentioned the
// base colour, so the balance-heuristic weights were constants of Task 2's
// differentiation. Both `pdf` and `pdfBsdfAtEnvDir` depend on roughness
// (through D and G1) and on metallic (through the lobe-selection probability
// q), so both weights move, and the term is written out below.
//
// A TERM TASK 2 ALSO DID NOT HAVE: d(1/p_B)/dtheta. The BSDF strategy's own
// estimator is f*cos*L*V / p_B and p_B is the mixture density -- which at
// metallic 0 / specularWeight 0 was NdotL/pi, free of the base colour, and is
// not free of roughness or metallic. So the quotient must be differentiated,
// not just its numerator. Writing it through the weight,
// weight = f*cos/p_B, keeps that automatic:
//
//     d(weight)/dtheta = ( d(f*cos)/dtheta - weight * d(p_B)/dtheta ) / p_B
//
// which is the quotient rule with the traversal's own `weight` substituted
// back in, so no re-derivation of f*cos/p_B can drift from the value the
// forward pass actually multiplied throughput by.
//
// ---------------------------------------------------------------------------
// WHERE EVERY FORMULA BELOW COMES FROM
// ---------------------------------------------------------------------------
//
// Each derivative is taken from the PUBLISHED expression, cited above it, and
// NOT by transcribing shaders/includes/material/ggx_aniso.glsl. That
// distinction is the whole point: an adjoint differentiated from the same
// lines it is validated against agrees with them by construction and cannot
// fail. This project has shipped six checks of that shape. The forward CPU
// oracle in diff_gpu_probe.cpp is written the same way and against the same
// four papers, which is what makes check 20 a real test of `ggxDiso` and the
// Smith terms; the same discipline is what makes these derivatives testable.
//
//   D      -- Walter, Marschner, Li & Torrance, "Microfacet Models for
//             Refraction through Rough Surfaces", EGSR 2007, Eq. 33:
//
//               D = alpha^2 / (pi cos^4(t_h) (alpha^2 + tan^2(t_h))^2)
//
//             With c = cos(t_h) and tan^2 = (1-c^2)/c^2, the denominator's
//             cos^4 (alpha^2 + tan^2)^2 = (c^2(alpha^2 - 1) + 1)^2, so with
//             A = alpha^2 and `denom` = c^2(A-1)+1,
//
//               D = A / (pi denom^2)
//               dD/dA = (denom - 2 A c^2) / (pi denom^3)
//
//             (quotient rule; d(denom)/dA = c^2). This is the SAME algebraic
//             identity check 20 already tests between the paper's tan-form and
//             ggx_aniso.glsl's denom-form -- the derivative is taken on the
//             paper's quantity A = alpha^2, so it inherits that check.
//
//   Lambda -- Heitz, "Understanding the Masking-Shadowing Function in
//             Microfacet-Based BRDFs", JCGT 3(2), 2014, Eq. 72:
//
//               Lambda(w) = (-1 + sqrt(1 + alpha^2 tan^2(t_w))) / 2
//
//             With t = tan^2(t_w) and u = 1 + A t,
//
//               dLambda/dA = t / (4 sqrt(u))
//
//   G1     -- Heitz 2014 Eq. 43: G1 = 1/(1+Lambda), so
//               dG1/dA = -(dLambda/dA) G1^2.
//   G2     -- Heitz 2014 Eq. 99 (height-correlated):
//             G2 = 1/(1+Lambda_v+Lambda_l), so
//               dG2/dA = -(dLambda_v/dA + dLambda_l/dA) G2^2.
//
//   F      -- Schlick, "An Inexpensive BRDF Model for Physically-based
//             Rendering", CGF 13(3), 1994: F = F0 + (1-F0)(1-cos)^5. F0 is
//             the ONLY parameter-dependent part, so with p = (1-cos)^5,
//               dF/dF0 = 1 - p.
//             F0 = mix(0.04, baseColor, metallic) [Karis 2013], so
//               dF0/d(metallic) = baseColor - 0.04,  dF0/d(roughness) = 0.
//
//   f_s    -- Cook & Torrance 1982 / Walter et al. 2007 Eq. 20:
//               f_s = specScale D F G2 / (4 |N.V| |N.L|)
//             a product, so its derivative is the sum of three product-rule
//             terms, one per parameter-dependent factor (specScale and F for
//             metallic; D and G2 for roughness).
//
//   f_d    -- Lambert: f_d = baseColor (1-metallic) / pi, so
//               df_d/d(metallic) = -baseColor/pi,  df_d/d(roughness) = 0.
//
//   p_B    -- the VNDF density, Heitz, "Sampling the GGX Distribution of
//             Visible Normals", JCGT 7(4), 2018, Eq. 3:
//               D_V(H) = G1(V) max(0, V.H) D(H) / (N.V),
//             folded with Walter 2007 Eq. 14's reflection Jacobian
//             dw_h/dw_i = 1/(4 V.H); the (V.H) factors cancel and
//               p_spec = G1(V) D / (4 N.V).
//             Mixed with the cosine density by the lobe probability:
//               p = mix(p_diff, p_spec, q) = p_diff + q (p_spec - p_diff), so
//               dp/dtheta = (dq/dtheta)(p_spec - p_diff) + q dp_spec/dtheta.
//
//   q      -- this integrator's OWN sampling strategy, not a published one
//             (bsdf.glsl's header states it as a contract, and any q positive
//             wherever f is gives an unbiased estimator). Differentiating a
//             contract is differentiating the contract as written:
//               q_raw = specScale * maxF(|N.V|) * (1 - 0.9 roughness)
//               q_1   = mix(q_raw, 1, metallic) = q_raw + metallic(1 - q_raw)
//               q     = clamp(q_1, 0, 1)
//             giving dq_raw/d(roughness) = -0.9 specScale maxF, and
//             dq_raw/d(metallic) = (1-specularWeight) maxF (1-0.9 r)
//                                + specScale (dmaxF/d(metallic)) (1-0.9 r),
//             with dq/dtheta = dq_1/dtheta inside the clamp and 0 outside it.
//
//   alpha  -- alpha = roughness^2 (bsdf.glsl's and ggx_aniso.glsl's shared
//             convention), so A = alpha^2 = roughness^4 and
//               dA/d(roughness) = 4 roughness^3.
//
//   unpack -- pbr_unpack.glsl floors roughness at 0.01 and clamps metallic to
//             [0,1], so d(unpacked)/d(pushed) is 1 in the interior and 0 where
//             the floor/clamp engages. `DiffVertex` carries `rawRoughness` and
//             `rawMetallic` beside the unpacked pair precisely so this factor
//             is DECIDABLE from the fields rather than reconstructed from a
//             constant copied out of another file.
//
// ---------------------------------------------------------------------------
// THE BANDS. A derivative must mirror the rejection band of the function that
// produced the value it differentiates -- the rule the albedo half of this
// file states and got wrong once. `diffBsdfEvalDeriv` below mirrors
// `diffBsdfEval` statement for statement: the same `NdotL <= 1e-4 ||
// NdotV <= 1e-4` rejection (where f and pdf are PINNED to zero, so their
// derivatives are zero), and the same `q <= 0` early return (where the
// specular lobe is not evaluated at all, so neither is its derivative).
// ---------------------------------------------------------------------------

/// Which scalar parameter a run differentiates. Pushed as `pc.diffParam`.
///
/// 0 is the BASE COLOUR and is the DEFAULT, so every caller that predates this
/// task keeps Stage 1 Task 2's behaviour exactly: the closed-form throughput
/// term, no MIS-weight term, and no forward-mode tangent maintained in path
/// state. 1 and 2 are the two this task adds, and they are the two with no
/// closed form for the throughput -- see `DiffVertex::tangent`. 3 is Stage 1
/// Task 4's -- see the "STAGE 1 TASK 4" banner at the end of this file.
///
/// 4 IS THE FIRST THAT IS NOT A SCALAR: Stage 1 Task 5's emission TEXTURE, a
/// `ParamKind::Texture` whose gradient is w*h*channels floats rather than one.
/// It is the same physics as 3 -- an added, never-sampled-from emitted
/// radiance -- read through a bilinear reconstruction instead of out of a
/// push constant, so its per-vertex adjoint is 3's answer distributed over
/// four texels by the reconstruction's own weights. See the "STAGE 1 TASK 5"
/// banner at the end of this file.
const uint DIFF_PARAM_BASECOLOR = 0u;
const uint DIFF_PARAM_ROUGHNESS = 1u;
const uint DIFF_PARAM_METALLIC = 2u;
const uint DIFF_PARAM_EMISSION = 3u;
const uint DIFF_PARAM_EMISSION_TEXTURE = 4u;

/// WHICH PARAMETERS NEED THE FORWARD-MODE THROUGHPUT TANGENT (Stage 1 Task 3,
/// traverse.glsl's `psSetTangent` gate), stated as an ALLOW-LIST rather than
/// an exclusion, on purpose: this branch has now added two DIFF_PARAM_*
/// values in a row that must NOT enter that branch (Task 4's
/// DIFF_PARAM_EMISSION; Task 5's DIFF_PARAM_EMISSION_TEXTURE, which review
/// caught falling through the exclude-by-name gate silently because the
/// exclusion named 3 but not 4). An exclude-list is wrong by silent inclusion
/// every time a new `DIFF_PARAM_*` is added and this comment is not
/// remembered.
///
/// BOTH FORMS FAIL SILENTLY, AND AN EARLIER VERSION OF THIS DOC CLAIMED
/// OTHERWISE. It argued that an allow-list "can only fail in the louder
/// direction", because a wrongly EXCLUDED parameter would leave nothing
/// behind but a derivative in `diffBsdfWeightDeriv` that nothing reads --
/// dead weight, not a correctness gap. THAT IS FALSE, and the data path in
/// traverse.glsl is what shows it: `dWeight`/`dPdfBsdf` are declared and
/// ZERO-INITIALISED before the gate, computed only INSIDE it, and then
/// written to `vtx.dBsdfWeight`/`vtx.dPdf` UNCONDITIONALLY. `diffParam` is a
/// push constant, fixed for a whole dispatch and not a per-vertex quantity,
/// so a wrongly excluded parameter never calls `psSetTangent` even once: the
/// tangent stays at its zero seed and BOTH of those vertex fields are baked
/// in as 0 for every vertex of every path. `diffVertexGgxScatter` below then
/// READS them, for its s = B (BSDF-sampled) term -- while its s = E
/// (next-event) term stays CORRECT, because that term calls
/// `diffBsdfEvalDeriv` directly with the real parameter and never passes
/// through this predicate at all. So what a wrong EXCLUSION produces is a
/// SILENTLY PARTIAL gradient: the BSDF-sampled contribution and the
/// throughput term zeroed, the light-sampled term right, and a total wrong
/// by an amount nothing here bounds. That is not "louder" than the
/// exclude-list's failure -- it is the same silence, moved.
///
/// THE REAL ARGUMENT FOR AN ALLOW-LIST IS THEREFORE NOT ABOUT FAILURE
/// DIRECTION, and must not be read as one. It is that the set which NEEDS a
/// tangent is small and STRUCTURAL -- the next paragraph derives it, and it
/// has not grown in three tasks -- while the set which does not need one
/// grows with every parameter this branch adds. Stating the small set puts
/// membership in ONE reviewable function; stating the large one makes it an
/// obligation on every future author to remember a deny-list they have no
/// reason to be reading. A wrong exclusion costs exactly what a wrong
/// inclusion cost: silence. The allow-list buys VISIBILITY of the decision,
/// not a louder failure -- making the failure actually loud takes a check,
/// not a choice of list polarity.
///
/// ROUGHNESS and METALLIC are the only two, and the reason is structural, not
/// a list of what happens to be true today: they are the only parameters
/// `bsdf.glsl` reads to build the GGX VNDF's alpha or the lobe-selection
/// probability `q`, i.e. the only ones whose perturbation moves a sampled
/// direction, so they are the only ones whose per-bounce BSDF weight
/// `diffBsdfWeightDeriv` computes a nonzero derivative for. BASECOLOR's
/// throughput term is closed-form (`diffVertexThroughputAlbedoTerm` below);
/// EMISSION and EMISSION_TEXTURE are additive and never sampled from, so
/// their closed-form throughput derivative is exactly 0 at every vertex (see
/// each one's own banner in this file). This function is the ONE place that
/// decides membership; keep the gate in traverse.glsl calling it rather than
/// re-testing `diffParam` itself.
bool diffParamNeedsForwardTangent(uint diffParam) {
    return diffParam == DIFF_PARAM_ROUGHNESS || diffParam == DIFF_PARAM_METALLIC;
}

/// d(unpacked)/d(pushed) for `pbr_unpack.glsl`'s roughness floor and metallic
/// clamp, decided from the raw/unpacked pair rather than from a copy of the
/// constant.
///
/// ROUGHNESS: `unpackHitPbr` returns max(|raw|, 0.01) (after an offset branch
/// for values >= 10 that nothing here uses), so the floor engaged exactly when
/// the unpacked value differs from the pushed one -- which is what
/// `DiffVertex`'s note says is decidable and must not be inferred.
///
/// METALLIC: clamp(raw, 0, 1), whose derivative is 1 strictly inside and 0
/// strictly outside. At raw == 0 and raw == 1 the derivative is ONE-SIDED and
/// this returns 0, which is a deliberate refusal rather than a convention: a
/// central finite difference straddling either endpoint measures half the
/// true one-sided slope, so a gradient run must sit strictly inside, and
/// reporting 0 there makes a run that does not sit inside fail its gate loudly
/// instead of passing at half scale.
float diffUnpackJacobianRaw(float unpackedRoughness, float rawRoughness, float rawMetallic,
                            uint param) {
    if (param == DIFF_PARAM_ROUGHNESS) {
        return (unpackedRoughness == rawRoughness) ? 1.0 : 0.0;
    }
    if (param == DIFF_PARAM_METALLIC) {
        return (rawMetallic > 0.0 && rawMetallic < 1.0) ? 1.0 : 0.0;
    }
    return 1.0;  // the base colour is pushed raw, with no unpack between
}

/// The same, read off a `DiffVertex`, whose two material spaces are exactly
/// the pair this needs.
float diffUnpackJacobian(in DiffVertex v, uint param) {
    return diffUnpackJacobianRaw(v.roughness, v.rawRoughness, v.rawMetallic, param);
}

/// d(f)/dtheta and d(pdf)/dtheta at ONE direction, for theta = the UNPACKED
/// roughness or the UNPACKED metallic (the caller multiplies by
/// `diffUnpackJacobian` to reach the pushed parameter).
///
/// `f` is `diffBsdfEval`'s f -- WITHOUT the cosine -- and `pdf` is its MIXTURE
/// density, so this function's two outputs are the derivatives of exactly the
/// pair that function returns, evaluated at exactly its arguments and rejected
/// in exactly its bands. Every expression is the derivative of the published
/// formula cited in this section's header, not of the GLSL that implements it.
void diffBsdfEvalDeriv(vec3 N, vec3 V, vec3 L, vec3 baseColor, float roughness, float metallic,
                       float specularWeight, uint param, out vec3 df, out float dpdf) {
    df = vec3(0.0);
    dpdf = 0.0;

    const float NdotL = dot(N, L);
    const float NdotV = dot(N, V);
    // diffBsdfEval PINS f and pdf to zero in this band, so both derivatives
    // are zero here -- not "small", zero. Same threshold, same comparison.
    if (NdotL <= DIFF_BSDF_MIN_COS || NdotV <= DIFF_BSDF_MIN_COS) return;

    // --- The Lambertian lobe. f_d = baseColor (1 - metallic) / pi.
    if (param == DIFF_PARAM_METALLIC) {
        df = -baseColor * DIFF_BSDF_INV_PI;
    }
    // p_diff = NdotL/pi mentions no material parameter at all.

    const float q = diffBsdfSpecProb(N, V, baseColor, roughness, metallic, specularWeight);
    // diffBsdfEval RETURNS HERE. The specular lobe is not evaluated, so it
    // contributes neither a value nor a derivative.
    if (q <= 0.0) return;

    // --- Shared microfacet quantities, at the SAME arguments diffBsdfEval
    // builds them from.
    const float alpha = roughness * roughness;
    const float A = alpha * alpha;  // Walter 2007's alpha_g^2
    const vec3 H = normalize(V + L);
    const float NdotH = max(dot(N, H), 0.0);
    const float VdotH = max(dot(V, H), 0.0);

    const float D = ggxDiso(NdotH, alpha);
    const float lamV = smithLambdaGGX(NdotV, alpha);
    const float lamL = smithLambdaGGX(NdotL, alpha);
    const float G2 = 1.0 / (1.0 + lamV + lamL);
    const float G1 = 1.0 / (1.0 + lamV);
    const vec3 F0 = diffBsdfF0(baseColor, metallic);
    const vec3 F = diffBsdfSchlick(F0, VdotH);
    const float specScale = diffBsdfSpecScale(specularWeight, metallic);

    const float pdfDiff = NdotL * DIFF_BSDF_INV_PI;
    const float pdfSpec = G1 * D / (4.0 * NdotV);

    // The lobe-selection probability's PRE-CLAMP value and its factors, needed
    // for dq/dtheta. Rebuilt here from bsdf.glsl's stated contract rather than
    // returned by diffBsdfSpecProb, which reports only the clamped result.
    const float cosV = clamp(abs(NdotV), 0.0, 1.0);
    const vec3 Fv = diffBsdfSchlick(F0, cosV);
    const float FvMax = max(Fv.r, max(Fv.g, Fv.b));
    const float qRaw = specScale * FvMax * (1.0 - roughness * 0.9);
    // Spelled with `mix`, the way diffBsdfSpecProb spells it, and not as the
    // algebraically equal qRaw + metallic*(1-qRaw): the clamp test below is a
    // COMPARISON against 1, and at metallic == 1 the mix form is exactly 1
    // while the expanded form need not be. Using the same expression makes
    // "the clamp engaged" the same fact on both sides.
    const float q1 = mix(qRaw, 1.0, metallic);
    // clamp(q1, 0, 1): derivative 1 strictly inside, 0 on either saturated
    // side. `q > 0.0` above already establishes q1 > 0.
    const float dq1dclamp = (q1 < 1.0) ? 1.0 : 0.0;

    if (param == DIFF_PARAM_ROUGHNESS) {
        // A = alpha^2 = roughness^4.
        const float dA = 4.0 * roughness * roughness * roughness;
        // ggxDiso floors a2 at 1e-8; inside the floor D is constant in A.
        // Post-unpack roughness >= 0.01 makes A >= 1e-8 with equality only at
        // the floor itself, so this branch is the boundary case and not an
        // approximation of one.
        const float dAforD = (A > 1e-8) ? dA : 0.0;

        // Walter 2007 Eq. 33 in the denom-form: D = A / (pi denom^2).
        const float a2 = max(A, 1e-8);
        const float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
        const float dD = ((denom - 2.0 * a2 * NdotH * NdotH) /
                          (DIFF_BSDF_PI * denom * denom * denom)) *
                         dAforD;

        // Heitz 2014 Eq. 72: dLambda/dA = tan^2 / (4 sqrt(1 + A tan^2)).
        const float cV2 = NdotV * NdotV;
        const float cL2 = NdotL * NdotL;
        const float tV = max(0.0, 1.0 - cV2) / max(cV2, 1e-8);
        const float tL = max(0.0, 1.0 - cL2) / max(cL2, 1e-8);
        const float dLamV = (tV / (4.0 * sqrt(1.0 + A * tV))) * dA;
        const float dLamL = (tL / (4.0 * sqrt(1.0 + A * tL))) * dA;

        // Heitz 2014 Eq. 43 and Eq. 99.
        const float dG1 = -dLamV * G1 * G1;
        const float dG2 = -(dLamV + dLamL) * G2 * G2;

        // Walter 2007 Eq. 20. specScale and F do not depend on roughness, so
        // only D and G2 carry product-rule terms.
        df += (specScale * (dD * G2 + D * dG2) / (4.0 * NdotV * NdotL)) * F;

        // Heitz 2018 Eq. 3 with Walter 2007 Eq. 14's Jacobian.
        const float dPdfSpec = (dG1 * D + G1 * dD) / (4.0 * NdotV);
        const float dq = dq1dclamp * (1.0 - metallic) * (-0.9 * specScale * FvMax);
        dpdf = dq * (pdfSpec - pdfDiff) + q * dPdfSpec;
        return;
    }

    if (param == DIFF_PARAM_METALLIC) {
        // Schlick 1994: dF/dF0 = 1 - (1-cos)^5. Karis 2013's F0 mix gives
        // dF0/d(metallic) = baseColor - 0.04.
        const vec3 dF0 = baseColor - vec3(0.04);
        const float mH = clamp(1.0 - VdotH, 0.0, 1.0);
        const float pH = mH * mH * mH * mH * mH;
        const vec3 dF = dF0 * (1.0 - pH);
        // specScale = mix(specularWeight, 1, metallic).
        const float dSpecScale = 1.0 - specularWeight;

        // Walter 2007 Eq. 20: D and G2 are metallic-independent, so only
        // specScale and F carry product-rule terms.
        df += (D * G2 / (4.0 * NdotV * NdotL)) * (dSpecScale * F + specScale * dF);

        // dq/d(metallic). The channel that ATTAINS the max is the one whose
        // derivative the max carries; with a grey base colour every channel
        // attains it and this is unambiguous.
        const float mV = clamp(1.0 - cosV, 0.0, 1.0);
        const float pV = mV * mV * mV * mV * mV;
        const vec3 dFvAll = dF0 * (1.0 - pV);
        float dFvMax = dFvAll.r;
        if (Fv.g > Fv.r && Fv.g >= Fv.b) {
            dFvMax = dFvAll.g;
        } else if (Fv.b > Fv.r && Fv.b > Fv.g) {
            dFvMax = dFvAll.b;
        }

        const float dqRaw = (dSpecScale * FvMax + specScale * dFvMax) * (1.0 - roughness * 0.9);
        const float dq = dq1dclamp * (dqRaw * (1.0 - metallic) + (1.0 - qRaw));
        // p_spec has no metallic dependence at all, so the whole of dpdf
        // comes through the lobe-selection probability.
        dpdf = dq * (pdfSpec - pdfDiff);
        return;
    }
}

/// d(bsdfWeight)/d(the PUSHED parameter) and d(pdf)/d(the PUSHED parameter) at
/// the direction the BSDF drew -- the pair the TRAVERSAL needs to advance the
/// forward-mode throughput tangent, and the pair it hands the hook on
/// `DiffVertex` rather than letting the hook recompute them.
///
/// `weight` and `pdf` are the traversal's OWN values, passed in rather than
/// re-evaluated, so the quotient rule below closes over exactly the number
/// throughput was multiplied by:
///
///     weight = f*cos / pdf
///     d(weight)/dtheta = ( d(f)/dtheta * cos - weight * d(pdf)/dtheta ) / pdf
///
/// A non-positive `pdf` is the VNDF's below-horizon tail, where the traversal
/// set both weight and pdf to exactly zero: the contribution is zero for every
/// theta in a neighbourhood of this one, so its derivative is zero and not a
/// division.
void diffBsdfWeightDeriv(vec3 N, vec3 V, vec3 L, vec3 baseColor, float roughness, float metallic,
                         float specularWeight, float rawRoughness, float rawMetallic, vec3 weight,
                         float pdf, uint param, out vec3 dWeight, out float dPdf) {
    dWeight = vec3(0.0);
    dPdf = 0.0;
    if (pdf <= 0.0) return;

    vec3 df;
    float dpdfUnpacked;
    diffBsdfEvalDeriv(N, V, L, baseColor, roughness, metallic, specularWeight, param, df,
                      dpdfUnpacked);

    const float jac = diffUnpackJacobianRaw(roughness, rawRoughness, rawMetallic, param);
    df *= jac;
    dPdf = dpdfUnpacked * jac;

    const float NdotL = max(dot(N, L), 0.0);
    dWeight = (df * NdotL - weight * dPdf) / pdf;
}

/// THE WHOLE VERTEX CONTRIBUTION to dJ/dtheta for theta = the pushed roughness
/// or the pushed metallic. It is `DiffVertex`'s recursion in full, with the
/// two terms the albedo half could leave out now written in:
///
///     dJ_b/dtheta = (dT_b/dtheta) * Lr_b  +  T_b * (dLr_b/dtheta)
///
///     dLr/dtheta = SUM_s [ (dw_s/dtheta) * U_s  +  w_s * (dU_s/dtheta) ]
///
/// with U_s the strategy's own unweighted estimator f*cos*L*V/p_s -- the
/// values the traversal formed, carried on `v.envUnweighted` and
/// `v.bsdfUnweighted` rather than rebuilt here.
///
/// (dT_b/dtheta) IS NOT A CLOSED FORM HERE. The albedo half could write
/// b*T_b/albedo because a pure Lambertian per-bounce weight is exactly the
/// albedo; a microfacet weight f*cos/pdf has no such form, so the traversal
/// carries the tangent forward instead, by the product rule, in path state.
/// `v.tangent` is that value on arrival at this vertex. See
/// `PathStateField::TangentR`.
///
/// THE TWO STRATEGIES' COSINE ASYMMETRY is the albedo half's, unchanged and
/// for the same reason: `fEnv` came out of `diffBsdfEval` and carries NO
/// cosine, so cos_E is a genuine factor here; the BSDF strategy's cosine is
/// already inside `v.f` and therefore inside `v.bsdfWeight`, so nothing
/// multiplies a cosine in on that side.
///
/// THE MIS WEIGHTS. `misBalanceHeuristic(a, b) = a / max(a + b, 1e-6)`, so
/// with only the BSDF-side density depending on theta:
///
///   w_E = p_E / (p_E + p_B(envDir))
///     dw_E/dtheta = -(w_E / S_E) * dp_B(envDir)/dtheta,  S_E = p_E + p_B(envDir)
///   w_B = p_B(bsdfDir) / (p_B(bsdfDir) + p_E(bsdfDir))
///     dw_B/dtheta = p_E(bsdfDir) * (dp_B/dtheta) / S_B^2, S_B = p_B + p_E(bsdfDir)
///
/// BOTH ARE CODED AS ZERO where the heuristic's own 1e-6 floor is the active
/// branch of its `max`, but for two DIFFERENT reasons, and only one of them
/// is "the weight does not depend on the density":
///
///   - `dw_E`: TRUE zero. `w_E`'s numerator `p_E` does not depend on theta at
///     all (the NEE direction and its density are material-independent), so
///     in the floored band `w_E = p_E/1e-6` is theta-independent and its
///     derivative really is 0 regardless of the floor.
///   - `dw_B`: zero IN EFFECT, not because the dependency vanishes. In the
///     floored band `misBalanceHeuristic` returns `p_B/1e-6`, whose
///     derivative is `dp_B/1e-6` -- NOT zero, and dominated by the very
///     density (`p_B`) that moves with theta. The floor being active here
///     means `p_B + p_E(bsdfDir) < 1e-6`, i.e. `p_E(bsdfDir) ~ 0` (p_B > 0 or
///     the vertex would not reach this branch); an unlit-from-the-BSDF-
///     strategy's-partner-density texel is one where `v.bsdfRadiance` (or the
///     visibility) is itself driving the whole B-branch estimator to zero, so
///     the coded 0 agrees with the true derivative's CONTRIBUTION to `dLr`,
///     not with `dw_B` itself being zero. It is unreachable in a way that
///     makes the number right, not a case the formula above actually covers
///     -- band-mirroring this file's discipline elsewhere would require
///     computing `dp_B/1e-6` here, and the reason that is not done is this
///     paragraph, not "the weight does not depend on either density."
vec3 diffVertexGgxScatter(in DiffVertex v, uint param) {
    if (!v.hit) return vec3(0.0);

    const float jac = diffUnpackJacobian(v, param);

    vec3 dLr = vec3(0.0);

    // --- s = E, next event. Its direction, density, radiance and visibility
    // are all independent of the material, so the only moving parts are
    // f at envDir and the partner density p_B(envDir) inside w_E.
    if (v.envPdf > 0.0) {
        vec3 dfEnv;
        float dPdfBsdfAtEnvDir;
        diffBsdfEvalDeriv(v.normal, v.wo, v.envDir, v.baseColor, v.roughness, v.metallic,
                          v.specularWeight, param, dfEnv, dPdfBsdfAtEnvDir);
        dfEnv *= jac;
        dPdfBsdfAtEnvDir *= jac;

        const float cosE = max(dot(v.normal, v.envDir), 0.0);
        const vec3 dU = dfEnv * cosE * v.envRadiance * v.visEnv / v.envPdf;

        const float sE = v.envPdf + v.pdfBsdfAtEnvDir;
        const float dwE = (sE >= 1e-6) ? (-(v.wEnv / sE) * dPdfBsdfAtEnvDir) : 0.0;

        dLr += dwE * v.envUnweighted + v.wEnv * dU;
    }

    // --- s = B, BSDF sampling. `v.dBsdfWeight` and `v.dPdf` are the
    // traversal's own derivatives at `v.bsdfDir`, already through the unpack
    // Jacobian -- not recomputed here, for the reason every other field of
    // this struct exists.
    if (v.pdf > 0.0) {
        const vec3 dU = v.dBsdfWeight * v.bsdfRadiance * v.visBsdf;

        const float sB = v.pdf + v.pdfEnvAtBsdfDir;
        const float dwB = (sB >= 1e-6) ? (v.pdfEnvAtBsdfDir * v.dPdf / (sB * sB)) : 0.0;

        dLr += dwB * v.bsdfUnweighted + v.wBsdf * dU;
    }

    // J = SUM_b T_b * Lr_b, so BOTH factors of this summand contribute.
    return v.tangent * v.Lr + v.throughput * dLr;
}

// ===========================================================================
// STAGE 1 TASK 4 -- d(film)/d(EMISSION), THE SIMPLEST ADJOINT IN THE STAGE
// ===========================================================================
//
// WHY THIS IS THE PLUMBING TEST, NOT A THIRD PHYSICS PROBLEM.
//
// The forward hook (shaders/diff/wf_scatter.comp) now writes
//
//     filmContribution = throughput * (Lr + vec3(pc.emission))
//
// so J(emission) = SUM over pixels, channels, bounces of
// throughput_b(pixel) * (Lr_b(pixel) + emission). Differentiating termwise:
//
//     dJ/d(emission) = SUM_b [ (dT_b/d(emission)) * (Lr_b + emission)
//                             + T_b * (dLr_b/d(emission) + 1) ]
//
// and EVERY term but the last is IDENTICALLY ZERO, not merely small, for a
// reason that is a statement about the shader source rather than an
// approximation:
//
//   * dT_b/d(emission) = 0. The throughput recursion is
//     T_{b+1} = T_b * bsdfWeight_b, and bsdfWeight (f*cos/pdf) comes out of
//     diffBsdfSample/diffBsdfSampleDetached (shaders/includes/diff/bsdf.glsl),
//     which takes baseColor/roughness/metallic/specularWeight and NEVER
//     `pc.emission` -- grep the file and it is not a parameter of either
//     function. So the throughput a path arrives with does not depend on
//     emission at ANY bounce: the closed form is 0 and it is exact, not
//     approximate, at every theta. traverse.glsl's tangent-maintenance gate
//     (`diffParamNeedsForwardTangent(pc.diffParam)`, an ALLOW-list of just
//     ROUGHNESS and METALLIC -- see that function's banner above) makes that
//     closed form explicit -- this parameter carries NO forward-mode
//     tangent in path state, by exclusion, not merely by every value that
//     reaches it happening to be zero.
//   * dLr_b/d(emission) = 0. `Lr` is nee.glsl's MIS-combined estimate --
//     built from `neeTerm`/`bsdfTerm`, which read baseColor, the material,
//     the environment CDF and the shadow-ray visibility, and again never
//     `pc.emission`. The self-emission term is ADDED to the film alongside
//     `Lr` in the forward hook, never blended into the sum `Lr` itself is
//     built from -- see traverse.glsl's film note. So the MIS weights
//     (which live inside `Lr`'s own construction, not this file's business
//     to touch here) do not move either: `(dw_s/d(emission))` is zero for
//     the SAME reason it was zero for the base colour at metallic 0 -- the
//     quantity in question never reaches `pdf` or `pdfEnvMap` -- except here
//     it is zero UNCONDITIONALLY, for every material, because nothing about
//     `emission` ever reaches a density at all, sampled or evaluated.
//   * d(emission)/d(emission) = 1, trivially -- the one surviving term.
//
// So the whole of DiffVertex's recursion collapses to
//
//     dJ/d(emission) = SUM_b T_b
//
// -- the arrival throughput at every hit vertex, summed over channels and
// bounces. That is a CLOSED FORM, exact for every material and every bounce
// count, and it is what makes J(emission) EXACTLY LINEAR in the parameter:
// J = A + emission * B with A = SUM_b T_b*Lr_b and B = SUM_b T_b, NEITHER of
// which depends on emission. A central difference is therefore exact for
// EVERY step size, not merely a small one -- unlike Task 2's albedo, whose
// linearity held only up to the E_1/E_2 terms of a genuine polynomial (see
// that file's harness), emission's higher "coefficients" are not small, they
// are absent: there is no emission^2 term anywhere in this shader for a
// central difference to have truncation error against.
//
// AT THE VERTEX LEVEL, "the emitted term passes straight through" means
// exactly this: with `dL` the adjoint arriving at this vertex (Task 2's
// `v.adjoint`, seeded from `v.throughput` for this sum-of-film objective and
// unaffected by any of the above -- the propagate line `dL_next = dL *
// bsdfWeight` reads no emission either), the scatter contribution is
// `dL * d(emission_term)/d(emission) = dL * 1 = dL`. No BSDF derivative, no
// density, no cosine, no MIS weight, nothing to differentiate at all -- the
// incoming adjoint is scattered UNMODIFIED. That is deliberately the whole
// of this function's body, and is what makes this task a test of the
// PLUMBING (does the right float end up in the right arena element,
// unmangled) rather than of any calculus.
//
// WHY THIS PARAMETER NEEDS NO DETACHED INSTRUMENT, stated as the check this
// file's header asks every new parameter to pass. Task 3's detached
// finite-difference harness exists because perturbing roughness or metallic
// moves the sampled direction (bsdf.glsl reads both to build `alpha` and the
// lobe-selection probability `q`). Emission is read by NEITHER
// diffBsdfSample/diffBsdfSampleDetached NOR sampleEnvMap (grep both files:
// neither takes an emission argument), so a +/-h perturbation of it changes
// no draw and moves no direction at any bounce -- the property Task 2's
// plain common-random-number harness needs to be exact. `diff_gpu_probe.cpp`
// measures this rather than asserting it, the same way Task 3's instrument
// measured that roughness and metallic do NOT have this property: it diffs
// the vertex trace's origin/direction/hitT slots (the same bit-exact
// comparison `traceGeometryMismatches` performs for Task 3) between the
// emission +/-h renders and the centre one, under a PLAIN (non-frozen)
// perturbation, and requires zero mismatches.
vec3 diffVertexEmissionScatter(in DiffVertex v) {
    if (!v.hit) return vec3(0.0);
    return v.adjoint;
}

// ===========================================================================
// STAGE 1 TASK 5 -- THE TEXTURE SCATTER, AND THE ONE PLACE FORWARD AND
// ADJOINT ARE REQUIRED TO SHARE
// ===========================================================================
//
// THIS SECTION BREAKS THIS BRANCH'S OTHERWISE UNIFORM RULE, ON PURPOSE, AND A
// READER WILL ASSUME OTHERWISE UNLESS IT SAYS SO IN THE CODE. Every other
// adjoint on this branch was written from an INDEPENDENT source: Task 3's GGX
// derivatives are transcribed from the published microfacet formulas rather
// than differentiated off bsdf.glsl's statements, and Task 2's albedo
// derivative likewise. That independence is what makes the finite-difference
// gates evidence rather than tautology -- a derivative differentiated off the
// same expression tree it is compared against can be wrong in the same way as
// the thing it differentiates and still agree with it.
//
// A TEXTURE READ IS DIFFERENT, and the difference is not a matter of taste.
// The read is
//
//     E(uv) = SUM over i in {00,10,01,11} of w_i(uv) * texel_i
//
// and the adjoint scatters `dL * w_i` into each of the four. The w_i are the
// weights OF THAT RECONSTRUCTION. A second, independently written bilinear
// reconstruction -- a different rounding of the texel coordinate, a different
// edge rule, a half-texel offset placed differently -- is a DIFFERENT
// FUNCTION E'(uv), and its derivative is the derivative of E', not of E. It
// would not be an independent check of the same answer; it would be an exact
// answer to a question nobody asked. So the footprint and the weights come
// from ONE function, `diffBilinearFootprint` below, called by the forward
// read and by the adjoint scatter, and the ordering from one
// `diffTexelElementIndex`. THIS IS THE ONLY PLACE IN STAGE 1 WHERE SHARING IS
// REQUIRED RATHER THAN FORBIDDEN.
//
// WHAT IS STILL INDEPENDENTLY CHECKED, so that the sharing does not swallow
// the evidence with it:
//
//   * THE CONSERVATION IDENTITY. `SUM_i w_i == 1` for a bilinear
//     reconstruction, so the four scattered values must sum to the incoming
//     adjoint -- an identity that holds WHATEVER the individual weights are,
//     and therefore one the shared reconstruction cannot make true by
//     construction. diff_gpu_probe.cpp check 44 measures it against a
//     SEPARATE run of DIFF_PARAM_EMISSION, whose scatter (one atomicAdd of a
//     channel sum, no weights anywhere) shares no code with this at all.
//   * THE ELEMENT ORDERING. Check 44 predicts, from the HOST's own rule and
//     a constant-uv configuration, exactly which arena floats a known
//     footprint may touch, and requires every other float of the whole arena
//     to be exactly 0. That is absolute, not self-consistent: the host's
//     prediction does not come from this file.
//   * THE MAGNITUDE, PER ELEMENT. Check 45 perturbs one primal texel element
//     on the HOST and compares (J(+h) - J(-h)) / 2h against the single arena
//     float this scatter wrote for that element. That is what closes the
//     forward/adjoint loop: the two are equal only if the read and the
//     scatter agree about WHICH element and about the weight on it.
//
// WHAT NONE OF THEM CHECK, stated rather than left to be assumed: the UV MAP
// ITSELF. `diffEmissionTexUv` is a modelling choice (see the Push block), and
// the gradient this file computes is correct with respect to whatever uv the
// forward read used, because the two read the same one. A wrong uv map is a
// wrong SCENE, not a wrong derivative, and no check here claims otherwise.
//
// WHICH INSTRUMENT THIS PARAMETER NEEDS, and the STRUCTURAL argument that
// settles it. Task 3's detached finite difference exists because perturbing
// roughness or metallic moves the sampled direction: `bsdf.glsl` reads both
// to build the GGX VNDF's alpha and the lobe-selection probability q. The
// emission texture is read by `diffEmissionAt` below and by nothing else in
// this translation unit -- `diffBsdfSample`/`diffBsdfSampleDetached` and
// `sampleEnvMap` take no emission argument of any kind, and binding 11
// appears in neither -- so perturbing a texel moves no draw and no direction
// at any bounce, exactly as the uniform `pc.emission` scalar does not. PLAIN
// COMMON RANDOM NUMBERS ARE THEREFORE EXACT for this parameter, and the
// detached instrument is not merely unnecessary but wrong for it (it would
// freeze a material this parameter never reaches). Check 45 MEASURES it the
// way Task 4 did -- `traceGeometryMismatches` between the perturbed renders
// and the centre one, required to be exactly 0 -- but the structural argument
// is what actually closes it, because that trace record is overwritten each
// bounce and so covers bounces 0..N-2 only.
//
// THE ADJOINT ITSELF IS TASK 4'S, UNCHANGED. With `dL` the adjoint arriving
// at this vertex, the film contribution is `throughput * (Lr + E(uv))` and
// every term of DiffVertex's recursion except d(E)/d(texel) is identically
// zero for exactly the reasons the "STAGE 1 TASK 4" banner above gives, term
// by term -- nothing about an emitted radiance reaches a throughput, a
// density or an MIS weight, whether it came from a push constant or from a
// texture. So
//
//     dJ/d(texel_i[c]) = SUM over hit vertices of dL[c] * w_i(uv)
//
// and the whole of the new work is distributing one number over four texels
// by the reconstruction's own weights. That is why this task is the TEXTURE
// SCATTER and not a fourth physics problem: the calculus is Task 4's and the
// content is the indexing.
//
// THE PER-CHANNEL SPLIT, and why it is not the scalar branch's channel SUM. A
// grey scalar parameter feeds all three channels, so its gradient is one
// number and summing the three channel contributions is the chain rule for
// `vec3(pc.emission)`. A 3-channel texture has a SEPARATE parameter per
// channel, so channel c's adjoint goes to channel c's element and nowhere
// else. Reading `min(c, channels - 1)` is what makes a 1-channel texture grey
// -- three channels of adjoint landing on one element, which is again the
// chain rule for one value feeding all three -- with no second code path.

/// THE ELEMENT ORDERING FOR A TEXTURE PARAMETER, spelled once.
///
/// Row-major over texels, channels INTERLEAVED within a texel. It must equal
/// `ohao::diff::ParamShape::elementIndex` (ohao/diff/param/param_registry.hpp)
/// exactly, and naming each other in a comment is NOT the tie -- this branch
/// carries four runtime source-parsing ties precisely because that was tried
/// and was not enough. `diff_gpu_probe.cpp`'s `checkTexelOrderingTie()` READS
/// THE RETURN STATEMENT BELOW out of this file at startup and refuses to run
/// the probe unless it is, modulo whitespace, `(y * width + x) * channels + c`
/// -- and unless the C++ function agrees with that formula at every (x, y, c)
/// of a non-degenerate shape. Keep the spelling on one line and in this form.
///
/// `ParamShape::floatCount()` is w*h*c -- a COUNT, which implies no ordering
/// whatsoever. This function is the ordering; that one is only its size.
uint diffTexelElementIndex(uint width, uint channels, uint x, uint y, uint c) {
    return (y * width + x) * channels + c;
}

/// One bilinear footprint: the four texel coordinates a uv touches and the
/// four weights it touches them with.
struct DiffBilinearFootprint {
    uint x0;
    uint x1;
    uint y0;
    uint y1;
    float w00;  // (x0, y0)
    float w10;  // (x1, y0)
    float w01;  // (x0, y1)
    float w11;  // (x1, y1)
};

/// Clamp a floating texel coordinate into [0, size-1] and truncate.
///
/// CLAMP, NOT WRAP -- but conservation does not decide between them: wrapping
/// would ALSO keep SUM_i w_i exactly 1 at the border (it moves mass to the far
/// edge; it does not destroy it), so the identity holds either way. What
/// actually decides it is that clamp matches every other texel-centre
/// convention this subsystem uses (env_sampling.glsl's CDF, and the
/// `(i + 0.5) / size` convention `diffBilinearFootprint` below is built on),
/// and it matches diff_gpu_probe.cpp's `hostBilinearFootprint`, the
/// independent host predictor check 44's footprint half is measured against
/// -- a host that wrapped while this clamped would disagree at the border for
/// a reason that has nothing to do with either being wrong. The choice that
/// WOULD break the identity is a border-DROP (an out-of-bounds corner simply
/// omitted rather than folded onto its in-bounds neighbour): that removes
/// mass instead of moving it, so SUM_i w_i < 1 there.
///
/// MEASURED, not merely argued: check 45's element 23 is texel (x=3, y=1) of
/// this task's 4x3x3 texture -- the last column, where x1 clamps onto x0 --
/// and its finite difference agreed with the analytic gradient to 1.1e-05.
uint diffClampTexel(float coord, uint size) {
    return uint(clamp(coord, 0.0, float(size) - 1.0));
}

/// THE ONE BILINEAR RECONSTRUCTION. Called by the forward read and by the
/// adjoint scatter; see this section's banner for why that sharing is
/// required rather than forbidden.
///
/// Texel CENTRES at (i + 0.5) / size -- the convention env_sampling.glsl's CDF
/// and every other texel-centre statement in this subsystem uses -- so the
/// continuous coordinate of a uv is `uv * size - 0.5`, and the fractional part
/// of that is the interpolant.
DiffBilinearFootprint diffBilinearFootprint(vec2 uv, uint width, uint height) {
    const float cx = uv.x * float(width) - 0.5;
    const float cy = uv.y * float(height) - 0.5;
    const float bx = floor(cx);
    const float by = floor(cy);
    const float tx = cx - bx;
    const float ty = cy - by;

    DiffBilinearFootprint fp;
    fp.x0 = diffClampTexel(bx, width);
    fp.x1 = diffClampTexel(bx + 1.0, width);
    fp.y0 = diffClampTexel(by, height);
    fp.y1 = diffClampTexel(by + 1.0, height);
    fp.w00 = (1.0 - tx) * (1.0 - ty);
    fp.w10 = tx * (1.0 - ty);
    fp.w01 = (1.0 - tx) * ty;
    fp.w11 = tx * ty;
    return fp;
}

/// The uv this subsystem reads an emission texture at: an affine function of
/// the hit point's XZ, coefficients pushed. See traverse.glsl's Push block for
/// why the map is a caller's business and why a zero scale -- every vertex on
/// one texel footprint -- is a legitimate configuration rather than a
/// degenerate one.
vec2 diffEmissionTexUv(in DiffVertex v) {
    return vec2(v.position.x * pc.emissionUvScaleU + pc.emissionUvBiasU,
                v.position.z * pc.emissionUvScaleV + pc.emissionUvBiasV);
}

/// THE FORWARD READ. `E(uv)` -- what the FORWARD instantiation's hook adds to
/// the film at this vertex, and the function the scatter below is the
/// derivative of.
///
/// With no texture configured (`pc.emissionTexWidth`/`Height` 0, which is what
/// every caller that predates Stage 1 Task 5 pushes) it is the uniform
/// `pc.emission` scalar, so the forward hook writes bit-identically what it
/// wrote before this task existed.
vec3 diffEmissionAt(in DiffVertex v) {
    if (pc.emissionTexWidth == 0u || pc.emissionTexHeight == 0u ||
        pc.emissionTexChannels == 0u) {
        return vec3(pc.emission);
    }
    const DiffBilinearFootprint fp =
        diffBilinearFootprint(diffEmissionTexUv(v), pc.emissionTexWidth, pc.emissionTexHeight);
    vec3 result = vec3(0.0);
    for (uint c = 0u; c < 3u; ++c) {
        const uint ch = min(c, pc.emissionTexChannels - 1u);
        result[c] =
            fp.w00 * emissionTex.v[diffTexelElementIndex(pc.emissionTexWidth,
                                                         pc.emissionTexChannels, fp.x0, fp.y0, ch)] +
            fp.w10 * emissionTex.v[diffTexelElementIndex(pc.emissionTexWidth,
                                                         pc.emissionTexChannels, fp.x1, fp.y0, ch)] +
            fp.w01 * emissionTex.v[diffTexelElementIndex(pc.emissionTexWidth,
                                                         pc.emissionTexChannels, fp.x0, fp.y1, ch)] +
            fp.w11 * emissionTex.v[diffTexelElementIndex(pc.emissionTexWidth,
                                                         pc.emissionTexChannels, fp.x1, fp.y1, ch)];
    }
    return result;
}

/// THE SCATTER. `dL * w_i` into each of the four texels the forward read
/// touched, per channel.
///
/// `mayScatter` is the caller's arena/pixel guard, computed once in the hook
/// beside the scalar branch's so the two cannot come apart; the PER-ELEMENT
/// bound is applied here, in the subtraction form traverse.glsl's arena note
/// requires (`k < gradArenaFloats - gradAlbedoOffset` after establishing
/// `gradAlbedoOffset < gradArenaFloats`, never `base + k` which could wrap).
///
/// `gradAlbedoOffset` is the DIFFERENTIATED PARAMETER's gradient block base
/// despite its name -- checks 39, 40 and 42 already push roughness's,
/// metallic's and the emission scalar's through it -- and here it is the base
/// of a w*h*channels-float block rather than of a one-float one. That is the
/// whole of what "the first parameter that is not a scalar" changes on this
/// side: the same base, a real k.
void diffScatterEmissionTexture(in DiffVertex v, vec3 dL, bool mayScatter) {
    if (!mayScatter || !v.hit) return;
    if (pc.emissionTexWidth == 0u || pc.emissionTexHeight == 0u ||
        pc.emissionTexChannels == 0u) {
        return;
    }
    const DiffBilinearFootprint fp =
        diffBilinearFootprint(diffEmissionTexUv(v), pc.emissionTexWidth, pc.emissionTexHeight);
    const uint xs[4] = uint[4](fp.x0, fp.x1, fp.x0, fp.x1);
    const uint ys[4] = uint[4](fp.y0, fp.y0, fp.y1, fp.y1);
    const float ws[4] = float[4](fp.w00, fp.w10, fp.w01, fp.w11);
    const uint span = pc.gradArenaFloats - pc.gradAlbedoOffset;

    for (uint i = 0u; i < 4u; ++i) {
        for (uint c = 0u; c < 3u; ++c) {
            const uint ch = min(c, pc.emissionTexChannels - 1u);
            const uint k = diffTexelElementIndex(pc.emissionTexWidth, pc.emissionTexChannels,
                                                 xs[i], ys[i], ch);
            if (k < span) {
                atomicAdd(grad.v[pc.gradAlbedoOffset + k], ws[i] * dL[c]);
            }
        }
    }
}

#endif  // OHAO_DIFF_BSDF_ADJOINT_GLSL
