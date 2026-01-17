// brdf_common.glsl - Shared BRDF utilities for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/brdf/brdf_common.glsl
//
// Provides common BRDF utilities used by various BRDF implementations.

#ifndef OHAO_BRDF_COMMON_GLSL
#define OHAO_BRDF_COMMON_GLSL

#include "includes/common/math.glsl"

// =============================================================================
// Fresnel Functions
// =============================================================================

// Schlick's Fresnel approximation
// F0 = reflectance at normal incidence (base reflectivity)
// cosTheta = dot(N, V) or dot(H, V)
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Schlick-Roughness Fresnel (accounts for roughness in IBL)
// Reduces fresnel effect at glancing angles for rough surfaces
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Calculate F0 (base reflectivity) from IOR
float f0FromIOR(float ior) {
    float f = (ior - 1.0) / (ior + 1.0);
    return f * f;
}

// Calculate F0 for a metallic surface
// For dielectrics, F0 is typically around 0.04 (4%)
// For metals, F0 is the albedo color
vec3 calculateF0(vec3 albedo, float metallic) {
    vec3 dielectricF0 = vec3(0.04);
    return mix(dielectricF0, albedo, metallic);
}

// =============================================================================
// Visibility / Masking Functions
// =============================================================================

// Smith's masking function using GGX (height-correlated)
// Approximation by Heitz 2014
float smithGGXMasking(float NdotV, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotV2 = NdotV * NdotV;
    return (2.0 * NdotV) / (NdotV + sqrt(a2 + (1.0 - a2) * NdotV2));
}

// Smith's shadowing function using GGX
float smithGGXShadowing(float NdotL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotL2 = NdotL * NdotL;
    return (2.0 * NdotL) / (NdotL + sqrt(a2 + (1.0 - a2) * NdotL2));
}

// Combined Smith geometry function (masking + shadowing)
// G = G1(V) * G1(L)
float smithG(float NdotV, float NdotL, float roughness) {
    return smithGGXMasking(NdotV, roughness) * smithGGXShadowing(NdotL, roughness);
}

// =============================================================================
// Diffuse BRDF
// =============================================================================

// Lambertian diffuse (simplest model)
// Returns diffuse / PI for energy conservation
vec3 lambertianDiffuse(vec3 diffuseColor) {
    return diffuseColor * INV_PI;
}

// Disney/Burley diffuse BRDF
// More physically accurate diffuse with roughness-based falloff
vec3 burleyDiffuse(vec3 diffuseColor, float roughness, float NdotV, float NdotL, float LdotH) {
    float f90 = 0.5 + 2.0 * roughness * LdotH * LdotH;
    float lightScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotL, 5.0);
    float viewScatter = 1.0 + (f90 - 1.0) * pow(1.0 - NdotV, 5.0);
    return diffuseColor * INV_PI * lightScatter * viewScatter;
}

// =============================================================================
// Helper Structures
// =============================================================================

// Surface data for BRDF evaluation
struct BRDFSurface {
    vec3 position;      // World position
    vec3 normal;        // Surface normal (normalized)
    vec3 viewDir;       // View direction (normalized, from surface to camera)
    vec3 albedo;        // Base color
    float metallic;     // Metalness (0 = dielectric, 1 = metal)
    float roughness;    // Perceptual roughness (0 = smooth, 1 = rough)
    float ao;           // Ambient occlusion
    vec3 F0;            // Pre-computed base reflectivity
};

// Light data for BRDF evaluation
struct BRDFLight {
    vec3 direction;     // Light direction (normalized, from surface to light)
    vec3 color;         // Light color (pre-multiplied with intensity)
    float attenuation;  // Distance/cone attenuation
};

// Initialize BRDF surface data
BRDFSurface initBRDFSurface(vec3 position, vec3 normal, vec3 viewDir,
                             vec3 albedo, float metallic, float roughness, float ao) {
    BRDFSurface surface;
    surface.position = position;
    surface.normal = normalize(normal);
    surface.viewDir = normalize(viewDir);
    surface.albedo = albedo;
    surface.metallic = metallic;
    surface.roughness = max(roughness, 0.04); // Prevent division by zero
    surface.ao = ao;
    surface.F0 = calculateF0(albedo, metallic);
    return surface;
}

#endif // OHAO_BRDF_COMMON_GLSL
