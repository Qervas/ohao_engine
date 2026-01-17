// light_attenuation.glsl - Light attenuation functions for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/lighting/light_attenuation.glsl
//
// Provides various light falloff/attenuation functions.

#ifndef OHAO_LIGHTING_LIGHT_ATTENUATION_GLSL
#define OHAO_LIGHTING_LIGHT_ATTENUATION_GLSL

#include "includes/common/math.glsl"

// =============================================================================
// Distance Attenuation Functions
// =============================================================================

// Inverse square falloff (physically correct)
// Parameters:
//   distance: distance from light to surface
//   range: light range (maximum distance)
// Returns: attenuation factor (0 to 1)
float attenuationInverseSquare(float distance, float range) {
    // Prevent division by zero
    float d = max(distance, 0.001);
    // Simple inverse square law: 1 / d^2
    float attenuation = 1.0 / (d * d);
    // Clamp to range
    return attenuation * smoothstep(range, 0.0, distance);
}

// Quadratic falloff with smooth cutoff (UE4 style)
// Parameters:
//   distance: distance from light to surface
//   range: light range (maximum distance)
// Returns: attenuation factor (0 to 1)
float attenuationUE4(float distance, float range) {
    float d = distance / range;
    float d2 = d * d;
    float d4 = d2 * d2;
    float falloff = saturate(1.0 - d4);
    return falloff * falloff / (d * d + 1.0);
}

// Smooth quadratic falloff (current OHAO default)
// Parameters:
//   distance: distance from light to surface
//   range: light range (maximum distance)
// Returns: attenuation factor (0 to 1)
float attenuationSmooth(float distance, float range) {
    // Quadratic falloff with range
    float attenuation = 1.0 / (1.0 + (distance * distance) / (range * range));
    // Smooth edge falloff
    float edgeFactor = saturate(1.0 - distance / range);
    return attenuation * edgeFactor * edgeFactor;
}

// Linear falloff (simple but not physically accurate)
// Parameters:
//   distance: distance from light to surface
//   range: light range (maximum distance)
// Returns: attenuation factor (0 to 1)
float attenuationLinear(float distance, float range) {
    return saturate(1.0 - distance / range);
}

// Custom falloff with adjustable exponent
// Parameters:
//   distance: distance from light to surface
//   range: light range (maximum distance)
//   exponent: falloff exponent (1 = linear, 2 = quadratic, etc.)
// Returns: attenuation factor (0 to 1)
float attenuationCustom(float distance, float range, float exponent) {
    float normalized = saturate(1.0 - distance / range);
    return pow(normalized, exponent);
}

// =============================================================================
// Spotlight Cone Attenuation
// =============================================================================

// Calculate spotlight cone attenuation
// Parameters:
//   cosAngle: cos(angle between light direction and surface direction)
//   innerCone: cos(inner cone angle) - full intensity inside this angle
//   outerCone: cos(outer cone angle) - zero intensity outside this angle
// Returns: cone attenuation factor (0 to 1)
float attenuationSpotCone(float cosAngle, float innerCone, float outerCone) {
    // Smooth falloff from inner to outer cone
    return saturate((cosAngle - outerCone) / (innerCone - outerCone));
}

// Calculate spotlight cone attenuation with smooth falloff
// Parameters:
//   cosAngle: cos(angle between light direction and surface direction)
//   innerCone: cos(inner cone angle)
//   outerCone: cos(outer cone angle)
// Returns: smooth cone attenuation factor (0 to 1)
float attenuationSpotConeSmooth(float cosAngle, float innerCone, float outerCone) {
    float t = saturate((cosAngle - outerCone) / (innerCone - outerCone));
    // Apply smoothstep for nicer falloff
    return t * t * (3.0 - 2.0 * t);
}

// =============================================================================
// Combined Attenuation
// =============================================================================

// Calculate combined distance and cone attenuation for a spotlight
// Parameters:
//   distance: distance from light to surface
//   range: light range
//   cosAngle: cos(angle between light direction and surface direction)
//   innerCone: cos(inner cone angle)
//   outerCone: cos(outer cone angle)
// Returns: combined attenuation factor (0 to 1)
float attenuationSpot(float distance, float range,
                       float cosAngle, float innerCone, float outerCone) {
    float distAtten = attenuationSmooth(distance, range);
    float coneAtten = attenuationSpotCone(cosAngle, innerCone, outerCone);
    return distAtten * coneAtten;
}

// Calculate combined attenuation for a point light
// Parameters:
//   distance: distance from light to surface
//   range: light range
// Returns: attenuation factor (0 to 1)
float attenuationPoint(float distance, float range) {
    return attenuationSmooth(distance, range);
}

// =============================================================================
// Window Functions (for area lights - future use)
// =============================================================================

// Rectangular window function
// Used for area light falloff
float windowRectangle(vec2 uv) {
    vec2 d = abs(uv) - vec2(1.0);
    return 1.0 - saturate(max(d.x, d.y));
}

// Circular window function
float windowCircle(vec2 uv) {
    return 1.0 - saturate(length(uv) - 1.0);
}

#endif // OHAO_LIGHTING_LIGHT_ATTENUATION_GLSL
