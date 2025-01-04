#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

// Push constants for selection highlight
layout(push_constant) uniform PushConstants {
    vec4 highlightColor;
    float scaleOffset;
} push;

void main() {
    // Scale the vertex position outward along its normal
    vec3 scaledPos = inPosition + (inNormal * push.scaleOffset);
    vec4 worldPos = ubo.model * vec4(scaledPos, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPos;
}
