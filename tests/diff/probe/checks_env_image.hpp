// Check 74: THE ENVIRONMENT'S RADIANCE, READ FROM AN IMAGE RATHER THAN
// INVERTED OUT OF ITS SAMPLING DENSITY.
//
// WHY THE BINDING EXISTS, and the reason is sharper than the one
// shaders/includes/diff/nee.glsl gives. That header says the CDF carries only
// a grey channel, so inverting it returns the right luminance and the wrong
// chroma, and that fixing it means binding the image. True, but secondary.
// The load-bearing reason is that the inversion makes the RADIANCE and the
// SAMPLING DISTRIBUTION the same array -- and spec 6.3 differentiates the
// estimator at FIXED directions, so an environment PARAMETER needs the
// radiance to move while the density stays put. Through the CDF it cannot.
// Binding the image is what separates them, and therefore what makes an
// environment parameter possible at all.
//
// WHAT THIS CHECK ASSERTS. Rendering a GREY environment both ways must give
// the same film: the inversion is exact for grey (check 31 asserts that
// texel by texel against the image it was built from), so the image path is
// a different route to the same number. An agreement here says the new route
// arrives, the binning matches the one the density was taken from, and every
// check written before this binding existed is unaffected by its presence.
//
// AND THE CONTROL IS WHAT MAKES THAT NON-VACUOUS. If
// `diffEnvImageRadiance` returned its "no image" sentinel unconditionally --
// a wrong push constant, a buffer bound short, a guard inverted -- the films
// would agree PERFECTLY, because both runs would be taking the fallback. So
// a third render binds a DELIBERATELY WRONG image, scaled by two, and is
// required to DIFFER. That is the assertion that the image is read at all,
// and it is the defect this subsystem has now been bitten by three times: a
// check whose passing condition is also satisfied by the feature being
// absent.
//
// THE ENVIRONMENT IS NOT UNIFORM, on purpose. buildParityEnvironment's map
// varies in both axes with a 5:1 contrast, so a texel-binning error moves
// the radiance; on a constant map every binning agrees and the comparison
// would hold for a lookup that read the wrong texel every time.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkEnvImageRadiance(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
