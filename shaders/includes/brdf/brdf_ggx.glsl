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
    // Roughness-aware Fresnel: limits grazing angle boost for rough surfaces.
    // At roughness=0.95, max Fresnel = max(0.05, F0) ≈ F0 (no boost).
    // This matches EEVEE's integrated reflectance from BRDF LUT behavior.
    F = fresnelSchlickRoughness(HdotV, F0, roughness);

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

// The core of evaluateBRDF and evaluateBRDFSphere: diffuse toward L (normalized), the given
// single-scatter specular (per NdotL) and its Fresnel F.
vec3 evaluateBRDFCore(BRDFSurface surface, vec3 L, vec3 specular, vec3 F, vec3 lightColor) {
    vec3 N = surface.normal;
    vec3 V = surface.viewDir;

    float NdotL = max(dot(N, L), 0.0);

    // Early out for backfacing
    if (NdotL <= 0.0) {
        return vec3(0.0);
    }

    float NdotV = max(dot(N, V), EPSILON);

    // Energy conservation: diffuse gets what specular doesn't reflect
    vec3 kD = (vec3(1.0) - F) * (1.0 - surface.metallic);

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

vec3 evaluateBRDF(BRDFSurface surface, vec3 lightDir, vec3 lightColor) {
    const vec3 L = normalize(lightDir);
    // Single-scatter specular BRDF
    vec3 F;
    const vec3 specular = evaluateSpecularBRDF(surface.normal, surface.viewDir, L, surface.roughness, surface.F0, F);
    return evaluateBRDFCore(surface, L, specular, F, lightColor);
}

// A rectangle of uniform radiance `Le` (colour included), centred `center`, spanned by edges e1
// and e2 and emitting one-sided toward e1 x e2: the path tracer's area light (its type 3, the
// same Le = I / (4 pi r^2)). Diffuse is the rectangle's exact irradiance, Le * dot(N, J) with J
// the polygon's vector form factor 1/2 sum acos(vi . vi+1) normalize(vi x vi+1) (Lambert's edge
// integral; the part below the horizon is not clipped, only the total clamped at 0). Specular is
// the representative point (the reflection ray where it meets the rectangle, clamped onto it)
// with the GGX lobe widened by the rectangle's projected size as a disc of equal area,
// a' = a + r_eq / (2 d): on a mirror the rectangle's reflection at Le times F, its emitter mesh.
vec3 evaluateBRDFRect(BRDFSurface surface, vec3 P, vec3 center, vec3 e1, vec3 e2, vec3 Le) {
    const vec3 nArea = cross(e1, e2);
    const float area = length(nArea);
    if (area <= 0.0) return vec3(0.0);
    const vec3 n = nArea / area;
    if (dot(P - center, n) <= 0.0) return vec3(0.0);   // behind the rectangle: it emits one side
    const vec3 corner = center - 0.5 * (e1 + e2);
    vec3 v[4];
    v[0] = normalize(corner - P);
    v[1] = normalize(corner + e1 - P);
    v[2] = normalize(corner + e1 + e2 - P);
    v[3] = normalize(corner + e2 - P);
    vec3 J = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        const vec3 c = cross(v[i], v[(i + 1) & 3]);
        const float s = length(c);
        if (s > 1e-7) J += acos(clamp(dot(v[i], v[(i + 1) & 3]), -1.0, 1.0)) * (c / s);
    }
    J *= 0.5;
    if (dot(J, center - P) < 0.0) J = -J;   // toward the rectangle, whichever way its corners wind
    const float E = dot(surface.normal, J);   // irradiance over Le
    if (E <= 0.0) return vec3(0.0);

    // The representative point: the reflection ray on the rectangle's plane, clamped onto it.
    const vec3 N = surface.normal, V = surface.viewDir;
    const vec3 R = reflect(-V, N);
    const float rn = dot(R, n);
    vec3 onPlane = center;
    if (rn < -1e-4) onPlane = P + R * (dot(center - P, n) / rn);
    const vec3 d = onPlane - corner;
    const vec3 rep = corner + clamp(dot(d, e1) / dot(e1, e1), 0.0, 1.0) * e1
                            + clamp(dot(d, e2) / dot(e2, e2), 0.0, 1.0) * e2 - P;
    const float dist = length(rep);
    const vec3 specL = rep / dist;
    const float cosLight = dot(-specL, n);
    vec3 F;
    vec3 specular = vec3(0.0);
    if (cosLight > 0.0) {
        const float a = surface.roughness * surface.roughness;
        const float rEq = sqrt(area * cosLight / PI);
        const float aRect = clamp(a + rEq / (2.0 * dist), 0.0, 1.0);
        // The rectangle as a source of intensity Le * area * cos toward the point.
        specular = evaluateSpecularBRDF(N, V, specL, sqrt(aRect), surface.F0, F)
                 * (area * cosLight / (dist * dist)) * max(dot(N, specL), 0.0) / E;
    } else {
        evaluateSpecularBRDF(N, V, normalize(J), surface.roughness, surface.F0, F);
    }
    // The core multiplies by Le * |J| * dot(N, J / |J|) = Le * E: diffuse gets the irradiance.
    return evaluateBRDFCore(surface, normalize(J), specular, F, Le * length(J));
}

// A sphere light of radius `radius` whose centre is `toLight` away (unnormalized): diffuse from
// the centre; specular from the representative point, the sphere's closest point to the
// reflection ray, with the GGX lobe widened by the sphere's angular size, a' = a + radius / (2 d)
// (Karis 2013, "Real Shading in Unreal Engine 4"). The lobe is evaluated at a' itself: its peak
// 1 / (pi a'^2) is the one Karis's D(a) * (a / a')^2 normalisation aims at, and D(a) at a mirror's
// a (~0.002) underflows distributionGGX's EPSILON floor. On a mirror that is the sphere's
// reflection at its radiance I / (pi r^2) times F: the light's emitter mesh, as the path
// tracer's specular ray sees it.
vec3 evaluateBRDFSphere(BRDFSurface surface, vec3 toLight, float radius, vec3 lightColor) {
    const vec3 L = normalize(toLight);
    const vec3 R = reflect(-surface.viewDir, surface.normal);
    const vec3 centerToRay = dot(toLight, R) * R - toLight;
    const vec3 closest = toLight + centerToRay * clamp(radius / max(length(centerToRay), EPSILON), 0.0, 1.0);
    const float a = surface.roughness * surface.roughness;
    const float aSphere = clamp(a + radius / (2.0 * length(toLight)), 0.0, 1.0);
    const vec3 specL = normalize(closest);
    vec3 F;
    vec3 specular = evaluateSpecularBRDF(surface.normal, surface.viewDir, specL, sqrt(aSphere), surface.F0, F);
    // The core multiplies by the centre's cosine; the lobe's is the representative point's.
    specular *= max(dot(surface.normal, specL), 0.0) / max(dot(surface.normal, L), EPSILON);
    return evaluateBRDFCore(surface, L, specular, F, lightColor);
}

// =============================================================================
// IBL Support Functions (deferred_lighting.frag's split-sum IBL)
// =============================================================================

// Pre-filtered environment map sampling parameters
// roughness is used to select mip level
float getRoughnessMipLevel(float roughness, float maxMipLevel) {
    return roughness * maxMipLevel;
}

// Split-sum approximation for IBL specular: the environment BRDF, analytic in place of a LUT
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
