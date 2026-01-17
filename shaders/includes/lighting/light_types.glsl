// light_types.glsl - Light data structures for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/lighting/light_types.glsl
//
// Defines light structures and common lighting utilities.
// For the main Light struct used with UBOs, see includes/common/types.glsl

#ifndef OHAO_LIGHTING_LIGHT_TYPES_GLSL
#define OHAO_LIGHTING_LIGHT_TYPES_GLSL

#include "includes/common/constants.glsl"

// =============================================================================
// Light Type Constants (duplicated from types.glsl for standalone use)
// =============================================================================

// Note: These are also defined in types.glsl. If both are included,
// the preprocessor will use the first definition.
#ifndef LIGHT_DIRECTIONAL
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2
#endif

// =============================================================================
// Light Helper Structures
// =============================================================================

// Simplified light data for BRDF evaluation
// Used after extracting data from the main Light UBO struct
struct LightParams {
    vec3 direction;     // Direction from surface to light (normalized)
    vec3 color;         // Light color (linear)
    float intensity;    // Light intensity
    float attenuation;  // Combined distance and cone attenuation
    int type;           // Light type (LIGHT_DIRECTIONAL, LIGHT_POINT, LIGHT_SPOT)
    bool castsShadow;   // Whether this light has shadows enabled
};

// =============================================================================
// Light Extraction Functions
// =============================================================================

// Extract light direction from surface to light
// Parameters:
//   lightPos: light position (world space)
//   lightDir: light direction vector (for directional lights)
//   lightType: LIGHT_DIRECTIONAL, LIGHT_POINT, or LIGHT_SPOT
//   surfacePos: surface position (world space)
// Returns: normalized direction from surface to light
vec3 getLightDirection(vec3 lightPos, vec3 lightDir, int lightType, vec3 surfacePos) {
    if (lightType == LIGHT_DIRECTIONAL) {
        // Directional light: direction is constant
        return normalize(-lightDir);
    } else {
        // Point/Spot light: direction is from surface to light
        return normalize(lightPos - surfacePos);
    }
}

// Get distance from surface to light
// Returns 0 for directional lights (infinite distance)
float getLightDistance(vec3 lightPos, int lightType, vec3 surfacePos) {
    if (lightType == LIGHT_DIRECTIONAL) {
        return 0.0; // Directional lights have no distance falloff
    } else {
        return length(lightPos - surfacePos);
    }
}

// =============================================================================
// Ambient Light
// =============================================================================

// Calculate ambient light contribution
// Parameters:
//   ambientIntensity: global ambient intensity
//   ao: ambient occlusion factor (0 = occluded, 1 = not occluded)
//   albedo: surface albedo
// Returns: ambient lighting contribution
vec3 calculateAmbient(float ambientIntensity, float ao, vec3 albedo) {
    return ambientIntensity * ao * albedo;
}

// =============================================================================
// Shadow Parameters
// =============================================================================

// Shadow parameters for a single light
struct ShadowParams {
    mat4 lightSpaceMatrix;  // World to light clip space transform
    float bias;             // Depth bias for shadow acne
    float strength;         // Shadow strength (0 = no shadow, 1 = full shadow)
    float normalBias;       // Normal-based bias
    int mapIndex;           // Shadow map index (-1 = no shadow)
};

// Check if a light casts shadows
bool lightCastsShadow(float shadowMapIndex) {
    return shadowMapIndex >= 0.0;
}

#endif // OHAO_LIGHTING_LIGHT_TYPES_GLSL
