// Check 75: GATE 5 FOR THE ENVIRONMENT -- its radiance recovered texel by
// texel, which is spec §10.3's `estimateLighting` becoming a real parameter
// rather than a stub returning baked constants.
//
// THE PARAMETER IS NOT A PROPERTY OF A SURFACE, which is what makes it
// different from every parameter gated before it. The base colour, roughness,
// metallic and both emission forms are read at a hit point; the environment
// is read along a DIRECTION, by two different strategies that sample two
// different directions, and the adjoint has to credit the texel each of them
// actually read. Two scatters per vertex, at generally different texels.
//
// THE SAMPLING DISTRIBUTION IS HELD FIXED, and that is the whole reason
// binding 14 exists. Spec §6.3 differentiates the estimator at FIXED
// directions, so the CDF this run importance-samples is built ONCE, from a
// UNIFORM environment, and never rebuilt as theta moves. The radiance comes
// from the image; the density comes from the CDF; they are different arrays
// and only one of them is the parameter.
//
// That makes the sampler UNINFORMED about the environment being recovered --
// it is importance-sampling a distribution that is not the integrand -- which
// costs variance and costs nothing in correctness. It is the honest
// configuration: a CDF rebuilt from theta every iteration would be a sampling
// density that depends on the parameter, and this adjoint does not carry that
// dependence.
//
// THE PARTITION IS READ OFF THE FIRST ITERATION'S GRADIENT, before any
// optimisation, exactly as check 55 does. A floor-only scene never sends a
// ray below the horizon, so the texels of the lower hemisphere are
// unconstrained by construction -- and they are separately required to be
// EXACTLY unmoved, because Adam at g = 0 has m = v = 0 and a step of
// alpha*0/(0+eps), which is zero and not merely small. An element that
// drifted would be the optimiser writing a parameter the scene does not
// depend on.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkEnvRecovery(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
