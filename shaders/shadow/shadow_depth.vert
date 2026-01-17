#version 450
#extension GL_GOOGLE_include_directive : require

// shadow_depth.vert - Shadow map depth pass vertex shader
// Part of OHAO Engine shader system
// Location: shadow/shadow_depth.vert
//
// Transforms vertices to light space for shadow map generation.

#include "includes/common/types.glsl"

// Vertex inputs - must match Vertex struct
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;      // unused but needed for vertex format
layout(location = 2) in vec3 inNormal;     // unused in shadow pass
layout(location = 3) in vec2 inTexCoord;   // unused in shadow pass

// Push constants - model matrix from ObjectPushConstants
layout(push_constant) uniform PushConstantData {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} pc;

// Light uniform buffer - binding 1
layout(binding = 1) uniform LightUniformBuffer {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    float shadowBias;
    float shadowStrength;
} lightUbo;

void main() {
    // Transform vertex to world space
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);

    // Find the first shadow-casting light and use its light space matrix
    mat4 lightSpaceMatrix = mat4(1.0);

    for (int i = 0; i < lightUbo.numLights && i < MAX_LIGHTS; ++i) {
        if (lightUbo.lights[i].params.z >= 0.0) {
            lightSpaceMatrix = lightUbo.lights[i].lightSpaceMatrix;
            break;
        }
    }

    // Transform to light clip space
    gl_Position = lightSpaceMatrix * worldPos;
}
