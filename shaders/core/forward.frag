#version 450
#extension GL_GOOGLE_include_directive : require

// forward.frag - Forward rendering fragment shader with PBR
// Part of OHAO Engine shader system
// Location: core/forward.frag
//
// Implements Cook-Torrance GGX BRDF with PCF shadows.
// Supports both Blinn-Phong (legacy) and PBR modes via defines.

// Include common types (defines Light struct, MAX_LIGHTS, light types)
#include "includes/common/types.glsl"

// Feature flags (can be set by engine at compile time)
#ifndef USE_PBR
#define USE_PBR 1  // Enable PBR by default
#endif

// Fragment inputs
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragPos;
layout(location = 3) in vec2 fragTexCoord;

// Fragment output
layout(location = 0) out vec4 outColor;

// Camera uniform (binding 0)
layout(binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} camera;

// Light uniform (binding 1)
// CRITICAL: Include files access this UBO directly by name "lighting"
layout(binding = 1) uniform LightUBO {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    float shadowBias;
    float shadowStrength;
} lighting;

// Per-object material (push constant)
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} material;

// Shadow map sampler (binding 2)
layout(binding = 2) uniform sampler2D shadowMap;

// Include shadow calculation AFTER defining the lighting UBO
#include "includes/shadow/shadow_pcf.glsl"

#if USE_PBR
// Include PBR BRDF for GGX lighting
#include "includes/common/math.glsl"
#include "includes/common/color.glsl"
#include "includes/brdf/brdf_ggx.glsl"
#include "includes/lighting/light_attenuation.glsl"
#else
// Include legacy Blinn-Phong lighting
#include "includes/lighting/blinn_phong.glsl"
#endif

// =============================================================================
// PBR Lighting Functions
// =============================================================================

#if USE_PBR

// Calculate light direction and attenuation for a light
void getLightParams(int lightIndex, vec3 fragPos, out vec3 lightDir, out float attenuation) {
    int lightType = int(lighting.lights[lightIndex].position.w);
    float range = lighting.lights[lightIndex].direction.w;

    if (lightType == LIGHT_DIRECTIONAL) {
        lightDir = normalize(-lighting.lights[lightIndex].direction.xyz);
        attenuation = 1.0;
    }
    else if (lightType == LIGHT_POINT) {
        vec3 toLight = lighting.lights[lightIndex].position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);
        attenuation = attenuationSmooth(distance, range);
    }
    else if (lightType == LIGHT_SPOT) {
        vec3 toLight = lighting.lights[lightIndex].position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);

        // Distance attenuation
        attenuation = attenuationSmooth(distance, range);

        // Spot cone attenuation
        float cosTheta = dot(lightDir, normalize(-lighting.lights[lightIndex].direction.xyz));
        float innerCone = lighting.lights[lightIndex].params.x;
        float outerCone = lighting.lights[lightIndex].params.y;
        attenuation *= attenuationSpotCone(cosTheta, innerCone, outerCone);
    }
    else {
        lightDir = vec3(0.0, 1.0, 0.0);
        attenuation = 0.0;
    }
}

// Evaluate PBR lighting for a single light
vec3 evaluatePBRLight(int lightIndex, BRDFSurface surface, float shadow) {
    vec3 lightDir;
    float attenuation;
    getLightParams(lightIndex, surface.position, lightDir, attenuation);

    vec3 lightColor = lighting.lights[lightIndex].color.rgb;
    float intensity = lighting.lights[lightIndex].color.w;

    // Combine light color with intensity
    vec3 radiance = lightColor * intensity * attenuation;

    // Evaluate BRDF
    vec3 contribution = evaluateBRDF(surface, lightDir, radiance);

    // Apply shadow
    return contribution * (1.0 - shadow);
}

#endif // USE_PBR

// =============================================================================
// Main Fragment Shader
// =============================================================================

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(camera.viewPos - fragPos);

#if USE_PBR
    // Initialize PBR surface data
    BRDFSurface surface = initBRDFSurface(
        fragPos,
        normal,
        viewDir,
        fragColor,              // albedo from vertex/material
        material.metallic,
        max(material.roughness, 0.04), // Prevent 0 roughness artifacts
        material.ao
    );

    // Ambient lighting (affected by AO)
    vec3 ambient = lighting.ambientIntensity * surface.albedo * surface.ao;

    // Accumulate light contributions
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < lighting.numLights && i < MAX_LIGHTS; i++) {
        // Calculate shadow for this light
        float shadow = calculateShadowForLightIndex(i, fragPos, normal, shadowMap);

        // Evaluate PBR lighting
        Lo += evaluatePBRLight(i, surface, shadow);
    }

    // Final color
    vec3 color = ambient + Lo;

    // HDR tonemapping (ACES)
    color = tonemapACES(color);

    // Gamma correction (linear to sRGB)
    color = linearToSRGBFast(color);

#else
    // Legacy Blinn-Phong path
    vec3 ambient = lighting.ambientIntensity * fragColor;

    vec3 lightingResult = vec3(0.0);
    for (int i = 0; i < lighting.numLights && i < MAX_LIGHTS; i++) {
        float shadow = calculateShadowForLightIndex(i, fragPos, normal, shadowMap);
        lightingResult += calculateBlinnPhongForLightIndex(i, fragPos, normal,
                                                           viewDir, fragColor, shadow, 32.0);
    }

    vec3 color = ambient + lightingResult;

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));
#endif

    outColor = vec4(color, 1.0);
}
