// uniforms.glsl - Unified shader uniforms
// This file contains camera data + unified lighting system
// CRITICAL: Structures must match C++ exactly

#ifndef UNIFORMS_GLSL
#define UNIFORMS_GLSL

#define MAX_LIGHTS 8
#define MAX_SHADOW_MAPS 4

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

    mat4 lightSpaceMatrix;   // Transform to light space for shadow mapping (64 bytes)
};

// Main uniform buffer - binding 0
// Contains camera data and unified lighting
layout(binding = 0) uniform GlobalUniformBuffer {
    // Camera matrices
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    float padding1;

    // Legacy single light (for compatibility during migration)
    vec3 lightPos;
    float padding2;
    vec3 lightColor;
    float lightIntensity;

    // Material properties (passed via push constants, kept for compatibility)
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;

    // Unified lighting system
    UnifiedLight lights[MAX_LIGHTS];  // 8 * 128 = 1024 bytes
    int numLights;
    float shadowBias;
    float shadowStrength;
    float padding5;

    // Legacy: Single light space matrix (for backward compatibility)
    mat4 lightSpaceMatrix;
} ubo;

// Shadow map array - binding 1
// Each shadow-casting light references an index into this array
layout(binding = 1) uniform sampler2D shadowMaps[MAX_SHADOW_MAPS];

// Light type constants
#define LIGHT_TYPE_DIRECTIONAL 0.0
#define LIGHT_TYPE_POINT 1.0
#define LIGHT_TYPE_SPOT 2.0

// Calculate shadow for a specific light
// Returns shadow factor: 0.0 = fully lit, shadowStrength = fully shadowed
float calculateShadowForLight(int lightIndex, vec3 worldPos, vec3 normal) {
    // Access shadow map index directly (avoid struct copy issue)
    int idx = ubo.lights[lightIndex].shadowMapIndex;

    // No shadow if light doesn't cast shadows
    if (idx < 0 || idx >= MAX_SHADOW_MAPS) {
        return 0.0;
    }

    // IMPORTANT: Use the SAME matrix as shadow_depth.vert uses for consistency
    // shadow_depth.vert uses ubo.lightSpaceMatrix (legacy)
    // We must use the same or shadows won't align
    vec4 lightSpacePos = ubo.lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC [-1,1] to texture coords [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Bounds check - outside light frustum = no shadow
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    // Get light direction for bias calculation
    vec3 lightDir;
    float lightType = ubo.lights[lightIndex].type;
    if (lightType == LIGHT_TYPE_DIRECTIONAL) {
        lightDir = normalize(-ubo.lights[lightIndex].direction);
    } else {
        lightDir = normalize(ubo.lights[lightIndex].position - worldPos);
    }

    // Calculate bias based on surface angle to light
    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = ubo.shadowBias * tan(acos(cosTheta));
    bias = clamp(bias, 0.0, 0.01);

    float currentDepth = projCoords.z;

    // PCF (Percentage Closer Filtering) for soft shadow edges
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMaps[idx], 0);

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 sampleCoord = projCoords.xy + vec2(x, y) * texelSize;
            float shadowDepth = texture(shadowMaps[idx], sampleCoord).r;
            shadow += (currentDepth - bias > shadowDepth) ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;  // Average of 3x3 samples

    return shadow * ubo.shadowStrength;
}

#endif // UNIFORMS_GLSL
