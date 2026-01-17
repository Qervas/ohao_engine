#version 450
#extension GL_GOOGLE_include_directive : require

#include "includes/offscreen_types.glsl"

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragPos;

layout(location = 0) out vec4 outColor;

// Camera uniform (binding 0)
layout(binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} camera;

// Light uniform (binding 1)
// CRITICAL: The include files access this UBO directly by name "lighting"
layout(binding = 1) uniform LightUBO {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    float shadowBias;
    float shadowStrength;
} lighting;

// Shadow map sampler (binding 2)
layout(binding = 2) uniform sampler2D shadowMap;

// Include shadow and lighting AFTER defining the UBO
// These functions access 'lighting' UBO directly to avoid struct copy corruption
#include "includes/offscreen_shadow.glsl"
#include "includes/offscreen_lighting.glsl"

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(camera.viewPos - fragPos);

    // Ambient
    vec3 ambient = lighting.ambientIntensity * fragColor;

    // Accumulate light contributions with shadows
    vec3 lightingResult = vec3(0.0);
    for (int i = 0; i < lighting.numLights && i < MAX_LIGHTS; i++) {
        // Calculate shadow for this light - pass INDEX, not struct!
        float shadow = calculateShadowForLightIndex(i, fragPos, normal, shadowMap);

        // Calculate light with shadow applied - pass INDEX, not struct!
        lightingResult += calculateBlinnPhongForLightIndex(i, fragPos, normal,
                                                           viewDir, fragColor, shadow, 32.0);
    }

    vec3 result = ambient + lightingResult;

    // Gamma correction
    result = pow(result, vec3(1.0 / 2.2));

    outColor = vec4(result, 1.0);
}
