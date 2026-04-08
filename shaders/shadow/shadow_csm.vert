#version 450

// Cascaded Shadow Map Vertex Shader
// Transforms vertices for shadow rendering, geometry shader will output to cascades

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in ivec4 inBoneIndices;
layout(location = 6) in vec4 inBoneWeights;

layout(location = 0) out vec3 outWorldPos;

// Per-object push constants (80 bytes — matches C++ ShadowPushConstant)
layout(push_constant) uniform ShadowPushConstants {
    mat4 model;          // 64 bytes
    uint cascadeIndex;   // 4 bytes
    float pad[3];        // 12 bytes
} object;

void main() {
    // Transform to world space - geometry shader will project to cascades
    outWorldPos = (object.model * vec4(inPosition, 1.0)).xyz;
    gl_Position = vec4(outWorldPos, 1.0);
}
