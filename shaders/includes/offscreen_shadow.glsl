// offscreen_shadow.glsl - Shadow calculation functions for offscreen renderer
// GLSL include file - use with glslangValidator -I flag
// Requires: offscreen_types.glsl
// IMPORTANT: This file accesses LightUBO directly to avoid struct copy corruption!

#ifndef OFFSCREEN_SHADOW_GLSL
#define OFFSCREEN_SHADOW_GLSL

// NOTE: Caller must define LightUBO before including this file!
// The UBO must have: lights[MAX_LIGHTS], shadowBias, shadowStrength

// Calculate shadow factor for a fragment
// CRITICAL: Pass light INDEX, not Light struct, to avoid GLSL struct copy corruption!
// Returns: 0.0 = fully lit, shadowStrength = fully shadowed
float calculateShadowForLightIndex(int lightIndex, vec3 worldPos, vec3 normal,
                                    sampler2D shadowMap) {
    // Access UBO members DIRECTLY - do NOT copy the Light struct!
    // Some GPU drivers corrupt large struct copies (the 128-byte Light struct)
    float shadowMapIndex = lighting.lights[lightIndex].params.z;

    // Only lights with valid shadow map index cast shadows
    if (shadowMapIndex < 0.0) {
        return 0.0;
    }

    // Access lightSpaceMatrix DIRECTLY from UBO - critical for correctness!
    mat4 lightSpaceMatrix = lighting.lights[lightIndex].lightSpaceMatrix;

    // Transform world position to light space
    vec4 lightSpacePos = lightSpaceMatrix * vec4(worldPos, 1.0);

    // Perspective divide
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC [-1,1] to texture coords [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Bounds check - outside light frustum = no shadow
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    // Get light direction for bias calculation - access UBO directly!
    vec3 lightDir;
    int lightType = int(lighting.lights[lightIndex].position.w);
    if (lightType == LIGHT_DIRECTIONAL) {
        lightDir = normalize(-lighting.lights[lightIndex].direction.xyz);
    } else {
        lightDir = normalize(lighting.lights[lightIndex].position.xyz - worldPos);
    }

    // Calculate bias based on surface angle to light (reduce shadow acne)
    float cosTheta = max(dot(normal, lightDir), 0.0);
    float bias = lighting.shadowBias * tan(acos(cosTheta));
    bias = clamp(bias, 0.0, 0.01);

    float currentDepth = projCoords.z;

    // Simple shadow test first (no PCF) for debugging
    float shadowDepth = texture(shadowMap, projCoords.xy).r;

    // Debug: Check if we're getting valid depth comparison
    // shadowDepth should be ~0.5 for rendered geometry
    // currentDepth should be similar for the same surface, slightly more for occluded surfaces

    // The shadow occurs when current fragment is BEHIND what was rendered to shadow map
    // i.e., currentDepth > shadowDepth (current point is further from light than shadow caster)
    float shadow = 0.0;
    if (currentDepth - bias > shadowDepth) {
        shadow = 1.0;
    }

    return shadow * lighting.shadowStrength;
}

#endif // OFFSCREEN_SHADOW_GLSL
