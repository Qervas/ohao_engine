#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform GizmoParams {
    mat4 viewProj;
    mat4 model;
    vec4 highlightColor;  // xyz = highlight color, w = highlight factor (0 or 1)
} params;

void main() {
    gl_Position = params.viewProj * params.model * vec4(inPosition, 1.0);
    // Mix base color with highlight
    fragColor = mix(inColor, params.highlightColor.xyz, params.highlightColor.w);
}
