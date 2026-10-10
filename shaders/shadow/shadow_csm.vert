#version 450
#extension GL_GOOGLE_include_directive : require

// Cascaded shadow map, depth only. The pass renders each cascade in its own
// render pass into its own array-layer view, so the vertex shader projects
// straight into that cascade: no geometry shader, no layer routing. The object's
// world matrix comes from RenderScene's instance table (set 0, indexed by the
// draw's firstInstance = slot); the cascade's view-projection rides in a 64-byte
// push constant.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec2 inTexCoord1;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

#include "includes/scene/scene_data.glsl"

// RenderScene's instance table; firstInstance = the slot.
layout(set = 0, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };

// Matches CSMPass::ShadowPushConstant: this cascade's light view-projection.
layout(push_constant) uniform ShadowPushConstants { mat4 viewProj; } pc;   // 64 bytes

void main() {
    gl_Position = pc.viewProj * instances[gl_InstanceIndex].world * vec4(inPosition, 1.0);
}
