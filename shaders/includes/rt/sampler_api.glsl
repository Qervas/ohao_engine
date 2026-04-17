#ifndef OHAO_SAMPLER_API_GLSL
#define OHAO_SAMPLER_API_GLSL

// Sampler dispatch — chosen at pipeline creation via Vulkan specialization
// constant. The SPIR-V optimizer folds the constant-id branch because
// SAMPLER_TYPE is known at compile time. Zero runtime cost.
//
// To add a sampler: define a new constant below, create sampler_<name>.glsl
// with functions suffixed _<name>, include it here, and add a branch.

#define SAMPLER_PCG   0u
#define SAMPLER_SOBOL 1u

// Default SAMPLER_PCG during Task 4 — Sobol include lands in Task 5.
// The CPU side overrides this value via VkSpecializationInfo in Task 6
// (which will default to Sobol for offline, PCG for realtime).
layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_PCG;

#include "includes/rt/sampler_pcg.glsl"

void samplerInit(uvec2 pixel, uint sampleIdx) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        samplerInit_pcg(pixel, sampleIdx);
    }
    // Sobol branch added in Task 5
}

// getSample1D / getSample2D: named to avoid collision with GLSL built-in
// sampler1D / sampler2D opaque types.
float getSample1D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return getSample1D_pcg(dim);
    }
    return 0.0;
}

vec2 getSample2D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return getSample2D_pcg(dim);
    }
    return vec2(0.0);
}

#endif
