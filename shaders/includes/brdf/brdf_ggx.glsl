// brdf_ggx.glsl - GGX/Cook-Torrance BRDF implementation
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/brdf/brdf_ggx.glsl
//
// Implements the Cook-Torrance microfacet BRDF with GGX distribution.
// This is the industry-standard PBR BRDF used in most modern game engines.
//
// References:
// - "Microfacet Models for Refraction" (Walter et al., 2007)
// - "Real Shading in Unreal Engine 4" (Karis, 2013)
// - "Moving Frostbite to PBR" (Lagarde & de Rousiers, 2014)

#ifndef OHAO_BRDF_GGX_GLSL
#define OHAO_BRDF_GGX_GLSL

#include "includes/brdf/brdf_common.glsl"

// =============================================================================
// GGX Normal Distribution Function (D)
// =============================================================================

// GGX/Trowbridge-Reitz NDF
// Describes the distribution of microfacet normals
// Parameters:
//   NdotH: dot product of normal and half-vector
//   roughness: perceptual roughness (will be squared internally)
// Returns: D term of the Cook-Torrance BRDF
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, EPSILON);
}

// =============================================================================
// GGX Geometry Function (G)
// =============================================================================

// Schlick-GGX geometry function (single direction)
// Approximates microfacet shadowing/masking
float geometrySchlickGGX(float NdotV, float roughness) {
    // Remapping for direct lighting (different from IBL)
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;
    return NdotV / max(denom, EPSILON);
}

// Smith's method combining shadowing and masking
// G = G1(V) * G1(L)
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float ggx1 = geometrySchlickGGX(NdotL, roughness);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    return ggx1 * ggx2;
}

// Height-correlated Smith-GGX (more accurate, slightly more expensive)
// Used by Frostbite, Unity HDRP
float geometrySmithCorrelated(float NdotV, float NdotL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;

    float lambdaV = NdotL * sqrt(a2 + (1.0 - a2) * NdotV * NdotV);
    float lambdaL = NdotV * sqrt(a2 + (1.0 - a2) * NdotL * NdotL);

    return 0.5 / max(lambdaV + lambdaL, EPSILON);
}

// =============================================================================
// Complete Cook-Torrance BRDF
// =============================================================================

// Evaluate the Cook-Torrance specular BRDF
// Returns the specular contribution for a single light
// Parameters:
//   N: surface normal
//   V: view direction (from surface to camera)
//   L: light direction (from surface to light)
//   roughness: perceptual roughness
//   F0: base reflectivity
// Returns: specular BRDF value and Fresnel term (for energy conservation)
vec3 evaluateSpecularBRDF(vec3 N, vec3 V, vec3 L, float roughness, vec3 F0, out vec3 F) {
    vec3 H = normalize(V + L);

    float NdotV = max(dot(N, V), EPSILON);
    float NdotL = max(dot(N, L), EPSILON);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Cook-Torrance components — height-correlated Smith-GGX (Frostbite/Blender)
    float D = distributionGGX(NdotH, roughness);
    float V_term = geometrySmithCorrelated(NdotV, NdotL, roughness);
    F = fresnelSchlick(HdotV, F0);

    return D * V_term * F;
}

// Evaluate the complete PBR BRDF (diffuse + specular)
// This is the main function for direct lighting
// Parameters:
//   surface: surface data (position, normal, albedo, metallic, roughness)
//   lightDir: light direction (from surface to light)
//   lightColor: light color (pre-multiplied with intensity and attenuation)
// Returns: final lit color contribution from this light
// Approximate directional albedo E(cosTheta, roughness) for single-scatter GGX.
// Polynomial fit from Turquin 2019 — no LUT needed.
// Returns the fraction of energy that single-scatter GGX preserves.
float GGX_E(float cosTheta, float roughness) {
    float a = roughness;
    float a2 = a * a;
    float mu = cosTheta;
    float e = 1.0 - (1.0 - mu) * (0.4022 * a2 + 0.7278 * a)
            - mu * (0.1412 * a2 + 0.1100 * a);
    return clamp(e, 0.0, 1.0);
}

vec3 evaluateBRDF(BRDFSurface surface, vec3 lightDir, vec3 lightColor) {
    vec3 N = surface.normal;
    vec3 V = surface.viewDir;
    vec3 L = normalize(lightDir);

    float NdotL = max(dot(N, L), 0.0);

    // Early out for backfacing
    if (NdotL <= 0.0) {
        return vec3(0.0);
    }

    float NdotV = max(dot(N, V), EPSILON);

    // Single-scatter specular BRDF
    vec3 F;
    vec3 specular = evaluateSpecularBRDF(N, V, L, surface.roughness, surface.F0, F);

    // Diffuse weight: metals have no diffuse. For dielectrics, Burley diffuse
    // has its own Fresnel handling internally — don't double-count with (1-F).
    // Using (1 - F0) instead of (1 - F) avoids the "wet rim" darkening at
    // grazing angles that makes dielectric skin look shiny.
    vec3 kD = (vec3(1.0) - surface.F0) * (1.0 - surface.metallic);

    // Burley/Disney diffuse (matches Blender's Principled BSDF)
    vec3 H = normalize(V + L);
    float LdotH = max(dot(L, H), 0.0);
    vec3 diffuse = kD * burleyDiffuse(surface.albedo, surface.roughness, NdotV, NdotL, LdotH);

    // Multi-scatter GGX energy compensation (Kulla-Conty 2017, Turquin 2019)
    // Single-scatter GGX loses energy at high roughness. The lost energy is
    // redistributed as additional diffuse-like scattering, making the specular
    // peak relatively less prominent. This matches Blender Cycles' multi-scatter GGX.
    float E_o = GGX_E(NdotV, surface.roughness);   // energy preserved at view angle
    float E_i = GGX_E(NdotL, surface.roughness);   // energy preserved at light angle
    float Ems = (1.0 - E_o) * (1.0 - E_i);        // energy lost by single scatter
    vec3 Favg = surface.F0 + (1.0 - surface.F0) / 21.0;  // average Fresnel
    vec3 fms = Favg * Ems / max(vec3(1.0) - Favg * (1.0 - E_o), vec3(0.001));

    // Add multi-scatter as diffuse-like lobe (already energy-conserving)
    diffuse += fms * INV_PI * (1.0 - surface.metallic) * surface.albedo;
    // For metals, multi-scatter adds colored specular energy
    specular += fms * surface.metallic;

    // Combined BRDF * cosTheta * lightColor
    return (diffuse + specular) * lightColor * NdotL;
}

// =============================================================================
// IBL Support Functions (for future Image-Based Lighting)
// =============================================================================

// Pre-filtered environment map sampling parameters
// roughness is used to select mip level
float getRoughnessMipLevel(float roughness, float maxMipLevel) {
    return roughness * maxMipLevel;
}

// Split-sum approximation for IBL specular
// Precomputed BRDF LUT lookup
// Returns: (scale, bias) for F0 * scale + bias
vec2 envBRDFApprox(float NdotV, float roughness) {
    // Karis approximation (avoids LUT texture lookup)
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// =============================================================================
// Utility: Complete Lighting with Shadow
// =============================================================================

// Convenience function to evaluate BRDF with shadow
// Parameters:
//   surface: surface data
//   lightDir: light direction
//   lightColor: light color (pre-multiplied with intensity)
//   attenuation: distance/cone attenuation
//   shadow: shadow factor (0 = lit, 1 = shadowed)
// Returns: final lit color
vec3 evaluateBRDFWithShadow(BRDFSurface surface, vec3 lightDir, vec3 lightColor,
                             float attenuation, float shadow) {
    vec3 contribution = evaluateBRDF(surface, lightDir, lightColor);
    return contribution * attenuation * (1.0 - shadow);
}

#endif // OHAO_BRDF_GGX_GLSL
