#version 450

// Cascaded shadow map, depth only. The pass renders each cascade in its own
// render pass into its own array-layer view, so the vertex shader projects
// straight into that cascade: no geometry shader, no layer routing, no
// descriptor set. The cascade's view-projection rides in push constants with
// the model matrix (128 bytes, the guaranteed push-constant minimum).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec2 inTexCoord1;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

// Matches CSMPass::ShadowPushConstant.
layout(push_constant) uniform ShadowPushConstants {
    mat4 model;
    mat4 viewProj;   // this cascade's light view-projection
} pc;

void main() {
    gl_Position = pc.viewProj * pc.model * vec4(inPosition, 1.0);
}
