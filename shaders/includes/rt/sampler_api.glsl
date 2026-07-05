#ifndef OHAO_SAMPLER_API_GLSL
#define OHAO_SAMPLER_API_GLSL

// Sampler dispatch — chosen at pipeline creation via Vulkan specialization
// constant. The SPIR-V optimizer folds the constant-id branch because
// SAMPLER_TYPE is known at compile time. Zero runtime cost.
//
// To add a sampler: define a new constant below, create sampler_<name>.glsl
// with getSample1D_<name> / getSample2D_<name> / samplerInit_<name>, include
// it here, and add a branch to the three dispatch functions below.

#define SAMPLER_PCG   0u
#define SAMPLER_SOBOL 1u

// Default SOBOL — CPU side overrides via VkSpecializationInfo (Task 6).
layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_SOBOL;

#include "includes/rt/sampler_pcg.glsl"
#include "includes/rt/sampler_sobol.glsl"

void samplerInit(uvec2 pixel, uint sampleIdx) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        samplerInit_pcg(pixel, sampleIdx);
    } else {
        samplerInit_sobol(pixel, sampleIdx);
    }
}

float getSample1D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return getSample1D_pcg(dim);
    }
    return getSample1D_sobol(dim);
}

vec2 getSample2D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return getSample2D_pcg(dim);
    }
    return getSample2D_sobol(dim);
}

#endif
