// shadow_types.glsl - Shadow data structures for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/shadow/shadow_types.glsl
//
// Defines shadow-related data structures and utilities.

#ifndef OHAO_SHADOW_TYPES_GLSL
#define OHAO_SHADOW_TYPES_GLSL

#include "includes/common/constants.glsl"

// =============================================================================
// Shadow Map Configuration
// =============================================================================

// Default shadow parameters
#define DEFAULT_SHADOW_BIAS      0.005
#define DEFAULT_SHADOW_STRENGTH  1.0
#define DEFAULT_NORMAL_BIAS      0.02
#define DEFAULT_PCF_RADIUS       2

// Shadow map resolution tiers
#define SHADOW_RES_LOW    512
#define SHADOW_RES_MEDIUM 1024
#define SHADOW_RES_HIGH   2048
#define SHADOW_RES_ULTRA  4096

// =============================================================================
// Shadow Data Structures
// =============================================================================

// Shadow map data for a single light
struct ShadowMapData {
    mat4 lightSpaceMatrix;  // World to light clip space transform
    vec4 params;            // x = bias, y = strength, z = normal bias, w = filter radius
    vec2 atlasOffset;       // Offset in shadow atlas (for atlas-based shadows)
    vec2 atlasScale;        // Scale in shadow atlas
};

// Cascaded Shadow Map data (for directional lights)
struct CascadedShadowData {
    mat4 lightSpaceMatrices[MAX_SHADOW_CASCADES];
    vec4 cascadeSplits;     // View-space split distances for each cascade
    vec4 params;            // x = bias, y = strength, z = normal bias, w = blend distance
    int numCascades;        // Number of active cascades (1-4)
    int padding[3];         // Alignment padding
};

// =============================================================================
// Shadow Calculation Utilities
// =============================================================================

// Transform world position to shadow map UV coordinates
// Parameters:
//   worldPos: world space position
//   lightSpaceMatrix: world to light clip space transform
// Returns: vec3 with xy = UV coordinates, z = depth
vec3 worldToShadowMapCoords(vec3 worldPos, mat4 lightSpaceMatrix) {
    vec4 lightSpacePos = lightSpaceMatrix * vec4(worldPos, 1.0);

    // Perspective divide
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC [-1,1] to texture coords [0,1]
    // Note: Vulkan uses [0,1] depth range, OpenGL uses [-1,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    return projCoords;
}

// Check if shadow map coordinates are valid (within [0,1] bounds)
bool isValidShadowCoord(vec3 shadowCoord) {
    return shadowCoord.x >= 0.0 && shadowCoord.x <= 1.0 &&
           shadowCoord.y >= 0.0 && shadowCoord.y <= 1.0 &&
           shadowCoord.z >= 0.0 && shadowCoord.z <= 1.0;
}

// Calculate depth bias based on surface angle to light
// Parameters:
//   normal: surface normal
//   lightDir: direction from surface to light
//   baseBias: base bias value
// Returns: adjusted bias value
float calculateSlopeBias(vec3 normal, vec3 lightDir, float baseBias) {
    float cosTheta = max(dot(normal, lightDir), 0.0);
    float tanTheta = tan(acos(cosTheta));
    float bias = baseBias * tanTheta;
    return clamp(bias, 0.0, baseBias * 10.0);
}

// Calculate normal offset for shadow mapping
// Offsets the position along the normal to reduce shadow acne
// Parameters:
//   worldPos: world position
//   normal: surface normal
//   lightDir: direction from surface to light
//   normalBias: bias scale
// Returns: offset world position
vec3 applyNormalOffset(vec3 worldPos, vec3 normal, vec3 lightDir, float normalBias) {
    float NdotL = dot(normal, lightDir);
    float offsetScale = (1.0 - NdotL) * normalBias;
    return worldPos + normal * offsetScale;
}

// =============================================================================
// Cascade Selection (for CSM)
// =============================================================================

// Select the appropriate cascade for a fragment
// Parameters:
//   viewSpaceZ: view-space Z coordinate (negative, looking down -Z)
//   cascadeSplits: view-space split distances for each cascade
//   numCascades: number of active cascades
// Returns: cascade index (0 to numCascades-1)
int selectCascade(float viewSpaceZ, vec4 cascadeSplits, int numCascades) {
    float depth = -viewSpaceZ; // Convert to positive depth

    for (int i = 0; i < numCascades; i++) {
        if (depth < cascadeSplits[i]) {
            return i;
        }
    }

    return numCascades - 1; // Fall back to last cascade
}

// Calculate cascade blend factor for smooth transitions
// Parameters:
//   viewSpaceZ: view-space Z coordinate
//   cascadeSplits: split distances
//   cascadeIndex: current cascade index
//   blendDistance: distance over which to blend
// Returns: blend factor (0 = use current cascade, 1 = use next cascade)
float getCascadeBlendFactor(float viewSpaceZ, vec4 cascadeSplits,
                             int cascadeIndex, float blendDistance) {
    if (cascadeIndex >= 3) {
        return 0.0; // No blending for last cascade
    }

    float depth = -viewSpaceZ;
    float splitDistance = cascadeSplits[cascadeIndex];
    float blendStart = splitDistance - blendDistance;

    if (depth > blendStart) {
        return (depth - blendStart) / blendDistance;
    }

    return 0.0;
}

#endif // OHAO_SHADOW_TYPES_GLSL
