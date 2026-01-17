#ifndef SHADOW_CSM_GLSL
#define SHADOW_CSM_GLSL

// Cascaded Shadow Map Sampling
// Supports 4 cascades with PCF filtering and cascade blending

#define NUM_CASCADES 4
#define PCF_SAMPLES 16
#define CASCADE_BLEND_WIDTH 0.1

// Cascade data UBO
struct CascadeData {
    mat4 viewProj[NUM_CASCADES];
    vec4 splitDepths;           // View-space split distances
    float blendWidth;
    float shadowBias;
    float normalBias;
    float padding;
};

// Poisson disk for PCF sampling
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2(0.94558609, -0.76890725),
    vec2(-0.094184101, -0.92938870),
    vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845),
    vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507),
    vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790)
);

// Select cascade based on view-space depth
int selectCascade(float viewDepth, vec4 splitDepths) {
    for (int i = 0; i < NUM_CASCADES - 1; ++i) {
        if (viewDepth < splitDepths[i]) {
            return i;
        }
    }
    return NUM_CASCADES - 1;
}

// Calculate shadow coordinates for a cascade
vec3 getShadowCoords(vec3 worldPos, mat4 lightSpaceMatrix) {
    vec4 shadowCoord = lightSpaceMatrix * vec4(worldPos, 1.0);
    shadowCoord.xyz /= shadowCoord.w;
    // Vulkan depth is already [0,1], but UV needs to be mapped from [-1,1] to [0,1]
    shadowCoord.xy = shadowCoord.xy * 0.5 + 0.5;
    return shadowCoord.xyz;
}

// PCF sampling for single cascade
float sampleShadowPCF(sampler2DArray shadowMap, int cascade, vec3 shadowCoords, float bias) {
    if (shadowCoords.x < 0.0 || shadowCoords.x > 1.0 ||
        shadowCoords.y < 0.0 || shadowCoords.y > 1.0 ||
        shadowCoords.z < 0.0 || shadowCoords.z > 1.0) {
        return 1.0; // Outside shadow map
    }

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float shadow = 0.0;

    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 offset = poissonDisk[i] * texelSize * 2.0;
        float depth = texture(shadowMap, vec3(shadowCoords.xy + offset, float(cascade))).r;
        shadow += (shadowCoords.z - bias > depth) ? 0.0 : 1.0;
    }

    return shadow / float(PCF_SAMPLES);
}

// Sample cascaded shadow map with cascade blending
float sampleCSM(sampler2DArray shadowMap, vec3 worldPos, vec3 normal,
                CascadeData cascadeData, float viewDepth) {
    // Select primary cascade
    int cascade = selectCascade(viewDepth, cascadeData.splitDepths);

    // Calculate shadow coordinates
    vec3 shadowCoords = getShadowCoords(worldPos, cascadeData.viewProj[cascade]);

    // Apply normal bias
    vec3 normalBias = normal * cascadeData.normalBias * (1.0 / float(cascade + 1));
    vec3 biasedWorldPos = worldPos + normalBias;
    shadowCoords = getShadowCoords(biasedWorldPos, cascadeData.viewProj[cascade]);

    // Calculate depth bias based on slope
    float bias = cascadeData.shadowBias * (float(cascade + 1) * 0.5);

    float shadow = sampleShadowPCF(shadowMap, cascade, shadowCoords, bias);

    // Blend between cascades at boundaries
    if (cascade < NUM_CASCADES - 1) {
        float splitDepth = cascadeData.splitDepths[cascade];
        float blendStart = splitDepth * (1.0 - cascadeData.blendWidth);

        if (viewDepth > blendStart) {
            // Sample next cascade
            vec3 nextShadowCoords = getShadowCoords(biasedWorldPos, cascadeData.viewProj[cascade + 1]);
            float nextBias = cascadeData.shadowBias * (float(cascade + 2) * 0.5);
            float nextShadow = sampleShadowPCF(shadowMap, cascade + 1, nextShadowCoords, nextBias);

            // Blend factor
            float blendFactor = (viewDepth - blendStart) / (splitDepth - blendStart);
            shadow = mix(shadow, nextShadow, blendFactor);
        }
    }

    return shadow;
}

// PCSS (Percentage Closer Soft Shadows) for CSM
float sampleCSM_PCSS(sampler2DArray shadowMap, vec3 worldPos, vec3 normal,
                      CascadeData cascadeData, float viewDepth, float lightSize) {
    int cascade = selectCascade(viewDepth, cascadeData.splitDepths);
    vec3 shadowCoords = getShadowCoords(worldPos, cascadeData.viewProj[cascade]);

    if (shadowCoords.x < 0.0 || shadowCoords.x > 1.0 ||
        shadowCoords.y < 0.0 || shadowCoords.y > 1.0) {
        return 1.0;
    }

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);

    // Step 1: Blocker search
    float avgBlockerDepth = 0.0;
    int blockerCount = 0;
    float searchRadius = lightSize * texelSize.x * 10.0;

    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 offset = poissonDisk[i] * searchRadius;
        float depth = texture(shadowMap, vec3(shadowCoords.xy + offset, float(cascade))).r;
        if (depth < shadowCoords.z) {
            avgBlockerDepth += depth;
            blockerCount++;
        }
    }

    if (blockerCount == 0) {
        return 1.0; // No blockers, fully lit
    }

    avgBlockerDepth /= float(blockerCount);

    // Step 2: Penumbra estimation
    float penumbraRatio = (shadowCoords.z - avgBlockerDepth) / avgBlockerDepth;
    float filterRadius = penumbraRatio * lightSize * texelSize.x * 20.0;
    filterRadius = clamp(filterRadius, texelSize.x, texelSize.x * 10.0);

    // Step 3: PCF with variable filter size
    float shadow = 0.0;
    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 offset = poissonDisk[i] * filterRadius;
        float depth = texture(shadowMap, vec3(shadowCoords.xy + offset, float(cascade))).r;
        shadow += (shadowCoords.z - cascadeData.shadowBias > depth) ? 0.0 : 1.0;
    }

    return shadow / float(PCF_SAMPLES);
}

#endif // SHADOW_CSM_GLSL
