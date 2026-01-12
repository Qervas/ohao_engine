// pcss.glsl - Percentage Closer Soft Shadows
// Contact-hardening shadows: sharp near occluders, soft far away
//
// Algorithm:
// 1. Blocker search - find average depth of blockers
// 2. Penumbra estimation - calculate penumbra size based on blocker distance
// 3. Variable-size PCF - sample with filter size based on penumbra
//
// CONSTANTS: Must match shader_bindings.hpp values
// - PCSS_BLOCKER_SEARCH_SAMPLES: 16
// - PCSS_PCF_SAMPLES: 25
// - PCSS_DEFAULT_LIGHT_SIZE: 0.04

#ifndef PCSS_GLSL
#define PCSS_GLSL

// ============================================================================
// CONFIGURATION
// ============================================================================

// Number of samples for blocker search (should match PCSS::kBlockerSearchSamples)
#define PCSS_BLOCKER_SEARCH_SAMPLES 16

// Number of samples for PCF filtering (should match PCSS::kPCFSamples)
#define PCSS_PCF_SAMPLES 25

// Default light size for penumbra calculation
#define PCSS_DEFAULT_LIGHT_SIZE 0.04

// Maximum and minimum penumbra size (in texels)
#define PCSS_MAX_PENUMBRA_SIZE 15.0
#define PCSS_MIN_PENUMBRA_SIZE 1.0

// ============================================================================
// SAMPLING PATTERNS
// ============================================================================

// Poisson disk samples for blocker search (16 samples)
const vec2 poissonDisk16[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

// Poisson disk samples for PCF (25 samples)
const vec2 poissonDisk25[25] = vec2[](
    vec2(-0.978698, -0.0884121),
    vec2(-0.841121,  0.521165),
    vec2(-0.71746,  -0.50322),
    vec2(-0.702933,  0.903134),
    vec2(-0.663198,  0.15482),
    vec2(-0.495102, -0.232887),
    vec2(-0.364238, -0.961791),
    vec2(-0.345866, -0.564379),
    vec2(-0.325663,  0.64037),
    vec2(-0.182714,  0.321329),
    vec2(-0.142613, -0.0227363),
    vec2(-0.0564287, -0.36729),
    vec2(-0.0185858,  0.918882),
    vec2( 0.0381787, -0.728996),
    vec2( 0.16599,   0.093112),
    vec2( 0.253639,  0.719535),
    vec2( 0.369549, -0.655019),
    vec2( 0.423627,  0.429975),
    vec2( 0.530747, -0.364971),
    vec2( 0.566027, -0.940489),
    vec2( 0.639332,  0.0284127),
    vec2( 0.652089,  0.669668),
    vec2( 0.773797,  0.345012),
    vec2( 0.968871,  0.840449),
    vec2( 0.991882, -0.657338)
);

// ============================================================================
// RANDOM ROTATION
// ============================================================================

// Generate rotation angle based on screen position to reduce banding
float getRotationAngle(vec2 screenPos) {
    // Use a simple hash function based on screen position
    return fract(sin(dot(screenPos, vec2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
}

// Rotate a 2D point around origin
vec2 rotatePoint(vec2 point, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return vec2(
        point.x * c - point.y * s,
        point.x * s + point.y * c
    );
}

// ============================================================================
// BLOCKER SEARCH
// ============================================================================

// Find average blocker depth in a search region
// Returns: x = average blocker depth, y = number of blockers found
vec2 findBlockerDepth(sampler2D shadowMap, vec2 uv, float receiverDepth,
                       float searchRadius, float bias, vec2 screenPos) {
    float blockerSum = 0.0;
    int blockerCount = 0;

    // Random rotation to reduce banding
    float angle = getRotationAngle(screenPos);

    for (int i = 0; i < PCSS_BLOCKER_SEARCH_SAMPLES; ++i) {
        vec2 offset = rotatePoint(poissonDisk16[i], angle) * searchRadius;
        vec2 sampleUV = uv + offset;

        // Bounds check
        if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 &&
            sampleUV.y >= 0.0 && sampleUV.y <= 1.0) {
            float shadowDepth = texture(shadowMap, sampleUV).r;

            // Is this sample a blocker?
            if (receiverDepth - bias > shadowDepth) {
                blockerSum += shadowDepth;
                blockerCount++;
            }
        }
    }

    if (blockerCount > 0) {
        return vec2(blockerSum / float(blockerCount), float(blockerCount));
    }

    return vec2(-1.0, 0.0); // No blockers found
}

// ============================================================================
// PENUMBRA ESTIMATION
// ============================================================================

// Estimate penumbra size based on blocker and receiver depths
float estimatePenumbraSize(float receiverDepth, float blockerDepth, float lightSize, float nearPlane) {
    // Geometric penumbra estimation
    // penumbra = lightSize * (receiverDepth - blockerDepth) / blockerDepth

    float penumbra = lightSize * (receiverDepth - blockerDepth) / max(blockerDepth, 0.001);

    // Clamp to reasonable range
    return clamp(penumbra, PCSS_MIN_PENUMBRA_SIZE, PCSS_MAX_PENUMBRA_SIZE);
}

// ============================================================================
// VARIABLE-SIZE PCF
// ============================================================================

// Perform PCF filtering with variable filter size
float pcfFilter(sampler2D shadowMap, vec2 uv, float receiverDepth,
                float filterRadius, float bias, vec2 screenPos) {
    float shadow = 0.0;

    // Random rotation to reduce banding
    float angle = getRotationAngle(screenPos);

    for (int i = 0; i < PCSS_PCF_SAMPLES; ++i) {
        vec2 offset = rotatePoint(poissonDisk25[i], angle) * filterRadius;
        vec2 sampleUV = uv + offset;

        // Bounds check
        if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 &&
            sampleUV.y >= 0.0 && sampleUV.y <= 1.0) {
            float shadowDepth = texture(shadowMap, sampleUV).r;
            shadow += (receiverDepth - bias > shadowDepth) ? 1.0 : 0.0;
        } else {
            // Outside shadow map = no shadow
            shadow += 0.0;
        }
    }

    return shadow / float(PCSS_PCF_SAMPLES);
}

// ============================================================================
// MAIN PCSS FUNCTION
// ============================================================================

/**
 * Calculate PCSS shadow factor
 *
 * @param shadowMap      Shadow map sampler
 * @param lightSpacePos  Fragment position in light clip space
 * @param lightSize      Physical light size (controls softness)
 * @param nearPlane      Light's near plane distance
 * @param bias           Shadow bias
 * @param screenPos      Screen position for random rotation (gl_FragCoord.xy)
 * @return Shadow factor (0.0 = lit, 1.0 = shadowed)
 */
float calculatePCSS(sampler2D shadowMap, vec4 lightSpacePos, float lightSize,
                     float nearPlane, float bias, vec2 screenPos) {
    // Perspective divide
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC to texture coords
    // Vulkan: x,y in [-1,1] -> [0,1], z already in [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Bounds check - outside frustum = no shadow
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    float receiverDepth = projCoords.z;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));

    // Step 1: Blocker search
    float searchRadius = lightSize * (receiverDepth - nearPlane) / receiverDepth;
    searchRadius = max(searchRadius, texelSize.x * 2.0); // Minimum search radius

    vec2 blockerInfo = findBlockerDepth(shadowMap, projCoords.xy, receiverDepth,
                                         searchRadius, bias, screenPos);

    // No blockers = fully lit
    if (blockerInfo.y == 0.0) {
        return 0.0;
    }

    float blockerDepth = blockerInfo.x;

    // Step 2: Penumbra estimation
    float penumbraSize = estimatePenumbraSize(receiverDepth, blockerDepth, lightSize, nearPlane);
    float filterRadius = penumbraSize * texelSize.x;

    // Step 3: Variable-size PCF
    return pcfFilter(shadowMap, projCoords.xy, receiverDepth, filterRadius, bias, screenPos);
}

/**
 * Calculate PCSS shadow with automatic light size based on light type
 *
 * @param shadowMap      Shadow map sampler
 * @param lightSpacePos  Fragment position in light clip space
 * @param bias           Shadow bias
 * @param screenPos      Screen position for random rotation
 * @return Shadow factor (0.0 = lit, 1.0 = shadowed)
 */
float calculatePCSSDefault(sampler2D shadowMap, vec4 lightSpacePos, float bias, vec2 screenPos) {
    return calculatePCSS(shadowMap, lightSpacePos, PCSS_DEFAULT_LIGHT_SIZE, 0.1, bias, screenPos);
}

// ============================================================================
// CASCADED SHADOW MAP SUPPORT
// ============================================================================

/**
 * Calculate PCSS for a specific CSM cascade
 *
 * @param cascadeShadowMap  Shadow map for this cascade
 * @param lightSpacePos     Fragment position in cascade's light space
 * @param cascadeIndex      Which cascade (for debug visualization)
 * @param lightSize         Physical light size
 * @param bias              Shadow bias
 * @param screenPos         Screen position for random rotation
 * @return Shadow factor (0.0 = lit, 1.0 = shadowed)
 */
float calculatePCSSCascade(sampler2D cascadeShadowMap, vec4 lightSpacePos,
                            int cascadeIndex, float lightSize, float bias, vec2 screenPos) {
    // Adjust light size based on cascade (larger cascades need larger light size)
    float adjustedLightSize = lightSize * float(1 << cascadeIndex);

    return calculatePCSS(cascadeShadowMap, lightSpacePos, adjustedLightSize, 0.1, bias, screenPos);
}

/**
 * Select cascade and calculate PCSS shadow for directional light
 *
 * @param cascadeShadowMaps Array of cascade shadow maps
 * @param cascadeMatrices   Light-space matrices for each cascade
 * @param cascadeSplits     View-space split depths
 * @param worldPos          Fragment world position
 * @param viewDepth         Fragment view-space depth
 * @param lightSize         Physical light size
 * @param bias              Shadow bias
 * @param screenPos         Screen position
 * @param numCascades       Number of active cascades
 * @return Shadow factor (0.0 = lit, 1.0 = shadowed)
 */
float calculatePCSSCSM(sampler2D cascadeShadowMaps[4], mat4 cascadeMatrices[4],
                        float cascadeSplits[4], vec3 worldPos, float viewDepth,
                        float lightSize, float bias, vec2 screenPos, int numCascades) {
    // Find the right cascade
    int cascadeIndex = 0;
    for (int i = 0; i < numCascades - 1; ++i) {
        if (viewDepth > cascadeSplits[i]) {
            cascadeIndex = i + 1;
        }
    }

    // Transform to cascade's light space
    vec4 lightSpacePos = cascadeMatrices[cascadeIndex] * vec4(worldPos, 1.0);

    // Calculate shadow with cascade-adjusted parameters
    return calculatePCSSCascade(cascadeShadowMaps[cascadeIndex], lightSpacePos,
                                 cascadeIndex, lightSize, bias, screenPos);
}

// ============================================================================
// SHADOW ATLAS SUPPORT
// ============================================================================

/**
 * Calculate PCSS for a shadow atlas tile
 *
 * @param atlasMap       Shadow atlas texture
 * @param lightSpacePos  Fragment position in light's clip space
 * @param uvOffset       UV offset for this tile in atlas
 * @param uvScale        UV scale for this tile
 * @param lightSize      Physical light size
 * @param bias           Shadow bias
 * @param screenPos      Screen position
 * @return Shadow factor (0.0 = lit, 1.0 = shadowed)
 */
float calculatePCSSAtlas(sampler2D atlasMap, vec4 lightSpacePos,
                          vec2 uvOffset, vec2 uvScale, float lightSize,
                          float bias, vec2 screenPos) {
    // Perspective divide
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Transform from NDC to tile UV coords
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Bounds check within tile
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 0.0;
    }

    // Transform to atlas UV space
    vec2 atlasUV = uvOffset + projCoords.xy * uvScale;

    float receiverDepth = projCoords.z;
    vec2 texelSize = uvScale / vec2(textureSize(atlasMap, 0));

    // Blocker search (within tile bounds)
    float searchRadius = lightSize * (receiverDepth - 0.1) / receiverDepth;
    searchRadius = clamp(searchRadius, texelSize.x * 2.0, uvScale.x * 0.25);

    // Sample within tile only
    float shadow = 0.0;
    float angle = getRotationAngle(screenPos);

    int validSamples = 0;
    for (int i = 0; i < PCSS_PCF_SAMPLES; ++i) {
        vec2 offset = rotatePoint(poissonDisk25[i], angle) * searchRadius;
        vec2 sampleUV = atlasUV + offset;

        // Check if sample is within tile bounds
        if (sampleUV.x >= uvOffset.x && sampleUV.x <= uvOffset.x + uvScale.x &&
            sampleUV.y >= uvOffset.y && sampleUV.y <= uvOffset.y + uvScale.y) {
            float shadowDepth = texture(atlasMap, sampleUV).r;
            shadow += (receiverDepth - bias > shadowDepth) ? 1.0 : 0.0;
            validSamples++;
        }
    }

    if (validSamples > 0) {
        return shadow / float(validSamples);
    }

    return 0.0;
}

#endif // PCSS_GLSL
