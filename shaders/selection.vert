#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

// Combined push constants
layout(push_constant) uniform PushConstants {
    // Model matrix (first 64 bytes)
    mat4 model;
    
    // Selection data (starts at offset 64)
    vec4 highlightColor;
    float scaleOffset;
} push;

void main() {
    // Scale the vertex position outward along its normal
    vec3 scaledPos = inPosition + (inNormal * push.scaleOffset);
    vec4 worldPos = push.model * vec4(scaledPos, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;
}
