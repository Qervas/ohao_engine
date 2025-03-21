#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

// Push constants for model matrix and selection highlight
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 highlightColor;
    float scaleOffset;
} push;

void main() {
    // Scale the vertex position outward along its normal
    vec3 scaledPos = inPosition + (inNormal * push.scaleOffset);

    // Use push constant model matrix
    vec4 worldPos = push.model * vec4(scaledPos, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;
}
