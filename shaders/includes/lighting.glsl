// lighting.glsl - Unified lighting system
// CRITICAL: This struct MUST match C++ UnifiedLight exactly (128 bytes)
// Changes here MUST be reflected in src/renderer/lighting/unified_light.hpp

#ifndef LIGHTING_GLSL
#define LIGHTING_GLSL

#define MAX_LIGHTS 8
#define MAX_SHADOW_MAPS 4

// Light types
#define LIGHT_TYPE_DIRECTIONAL 0.0
#define LIGHT_TYPE_POINT 1.0
#define LIGHT_TYPE_SPOT 2.0

// Unified light structure - 128 bytes exactly
// Light + Shadow data are ONE unit - if shadowMapIndex >= 0, shadow is valid
struct UnifiedLight {
    vec3 position;
    float type;              // 0=directional, 1=point, 2=spot

    vec3 color;
    float intensity;

    vec3 direction;
    float range;

    float innerCone;
    float outerCone;
    int shadowMapIndex;      // -1 = no shadow, >= 0 = index into shadow maps array
    float _padding;

    mat4 lightSpaceMatrix;   // Transform to light space for shadow mapping
};

// Main lighting UBO - binding 0
layout(set = 0, binding = 0) uniform LightingUBO {
    UnifiedLight lights[MAX_LIGHTS];
    int numLights;
    float shadowBias;
    float shadowStrength;
    float _padding;
} lighting;

// Shadow map array - binding 1
// Each shadow-casting light references an index into this array
layout(set = 0, binding = 1) uniform sampler2D shadowMaps[MAX_SHADOW_MAPS];

// Calculate shadow for a specific light
// Returns shadow factor: 0.0 = fully lit, shadowStrength = fully shadowed
float calculateShadowForLight(int lightIndex, vec3 worldPos, vec3 normal) {
    UnifiedLight light = lighting.lights[lightIndex];

    // No shadow if light doesn't cast shadows
    if (light.shadowMapIndex < 0) {
        return 0.0;
    }

    // Validate shadow map index
    if (light.shadowMapIndex >= MAX_SHADOW_MAPS) {
        return 0.0;
    }

    // Transform to light space using the light's own matrix
    vec4 lightSpacePos = light.lightSpaceMatrix * vec4(worldPos, 1.0);

    // Perspective divide
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC [-1,1] to texture coords [0,1]
    // Vulkan NDC: x,y in [-1,1], z in [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Bounds check - outside light frustum = no shadow
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    // Get light direction for bias calculation
    vec3 lightDir;
    if (light.type == LIGHT_TYPE_DIRECTIONAL) {
        lightDir = normalize(-light.direction);
    } else {
        lightDir = normalize(light.position - worldPos);
    }

    // Calculate bias based on surface angle to light
    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = lighting.shadowBias * tan(acos(cosTheta));
    bias = clamp(bias, 0.0, 0.01);

    float currentDepth = projCoords.z;

    // PCF (Percentage Closer Filtering) for soft shadow edges
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMaps[light.shadowMapIndex], 0);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 sampleCoord = projCoords.xy + vec2(x, y) * texelSize;
            float shadowDepth = texture(shadowMaps[light.shadowMapIndex], sampleCoord).r;
            shadow += (currentDepth - bias > shadowDepth) ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;  // Average of 3x3 samples

    return shadow * lighting.shadowStrength;
}

// Calculate shadow without PCF (faster, harder edges)
float calculateShadowForLightSimple(int lightIndex, vec3 worldPos, vec3 normal) {
    UnifiedLight light = lighting.lights[lightIndex];

    if (light.shadowMapIndex < 0 || light.shadowMapIndex >= MAX_SHADOW_MAPS) {
        return 0.0;
    }

    vec4 lightSpacePos = light.lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    float shadowDepth = texture(shadowMaps[light.shadowMapIndex], projCoords.xy).r;
    float bias = 0.005;

    return (projCoords.z - bias > shadowDepth) ? lighting.shadowStrength : 0.0;
}

#endif // LIGHTING_GLSL
