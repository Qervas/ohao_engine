#version 450

// Skinned G-Buffer Vertex Shader
// Applies skeletal animation (bone matrices) before G-Buffer output

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec2 inTexCoord1;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec2 fragTexCoord1;
layout(location = 5) out vec4 fragCurrentPos;
layout(location = 6) out vec4 fragPrevPos;

// Per-object push constants (matches GBufferUBO in C++)
// Total: 224 bytes (3 mat4 + 2 vec4) — fits within 256-byte NVIDIA limit
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;        // precomputed projection * view
    mat4 prevMVP;
    vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=albedoTexIdx (uint bits)
    vec4 albedoColor;
    vec4 emissiveParams;
} pc;

// Bone matrices UBO (set 1 so textures stay at set 0 — same fragment shader for all pipelines)
layout(set = 1, binding = 0) uniform BoneMatrices {
    mat4 bones[128];
    int boneCount;
} boneUBO;

void main() {
    // Compute skin matrix from bone influences (clamp to valid range)
    ivec4 bi = clamp(inBoneIndices, ivec4(0), ivec4(127));
    mat4 skinMatrix =
        boneUBO.bones[bi.x] * inBoneWeights.x +
        boneUBO.bones[bi.y] * inBoneWeights.y +
        boneUBO.bones[bi.z] * inBoneWeights.z +
        boneUBO.bones[bi.w] * inBoneWeights.w;

    // Apply skinning then model transform
    vec4 skinnedPos = skinMatrix * vec4(inPosition, 1.0);
    vec4 worldPos = pc.model * skinnedPos;
    fragWorldPos = worldPos.xyz;

    // Transform normal through skin matrix and model
    mat3 skinNormalMatrix = transpose(inverse(mat3(skinMatrix)));
    mat3 modelNormalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(modelNormalMatrix * skinNormalMatrix * inNormal);

    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;

    fragCurrentPos = pc.viewProj * worldPos;
    fragPrevPos = pc.prevMVP * skinnedPos;

    gl_Position = fragCurrentPos;
}
