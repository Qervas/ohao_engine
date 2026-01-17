#version 450

// Cascaded Shadow Map Vertex Shader
// Transforms vertices for shadow rendering, geometry shader will output to cascades

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 outWorldPos;

// Per-object push constants
layout(push_constant) uniform ObjectPushConstants {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} object;

void main() {
    // Transform to world space - geometry shader will project to cascades
    outWorldPos = (object.model * vec4(inPosition, 1.0)).xyz;
    gl_Position = vec4(outWorldPos, 1.0);
}
