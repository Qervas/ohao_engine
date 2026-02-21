#version 450

// Skinned Cascaded Shadow Map Vertex Shader
// Applies bone skinning before shadow map rendering

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in ivec4 inBoneIndices;
layout(location = 6) in vec4 inBoneWeights;

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

// Bone matrices UBO
layout(set = 0, binding = 0) uniform BoneMatrices {
    mat4 bones[128];
    int boneCount;
} boneUBO;

void main() {
    // Compute skin matrix
    mat4 skinMatrix =
        boneUBO.bones[inBoneIndices.x] * inBoneWeights.x +
        boneUBO.bones[inBoneIndices.y] * inBoneWeights.y +
        boneUBO.bones[inBoneIndices.z] * inBoneWeights.z +
        boneUBO.bones[inBoneIndices.w] * inBoneWeights.w;

    vec4 skinnedPos = skinMatrix * vec4(inPosition, 1.0);
    outWorldPos = (object.model * skinnedPos).xyz;
    gl_Position = vec4(outWorldPos, 1.0);
}
