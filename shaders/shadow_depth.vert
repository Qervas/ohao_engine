#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;      // unused but needed for vertex format
layout(location = 2) in vec3 inNormal;     // unused
layout(location = 3) in vec2 inTexCoord;   // unused

// Push constant for model matrix (reuse existing structure)
layout(push_constant) uniform PushConstantData {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} pc;

void main() {
    // Transform vertex to world space using model matrix from push constants
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);

    // Transform to light clip space using the light space matrix from UBO
    // This matrix contains: lightProjection * lightView
    gl_Position = ubo.lightSpaceMatrix * worldPos;
}
