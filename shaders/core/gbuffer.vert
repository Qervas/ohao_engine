#version 450
#extension GL_GOOGLE_include_directive : require

// G-Buffer Vertex Shader
// Transforms vertices and passes data to fragment shader for G-Buffer population

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
layout(location = 5) out vec4 fragTangent;
layout(location = 6) out vec4 fragCurrentPos;
layout(location = 7) out vec4 fragPrevPos;

#include "includes/scene/scene_data.glsl"
#include "shared/layout.glsl"

// Set 1 "scene": RenderScene's instance table, the frame slot's camera and the draw table.
layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 3) uniform Camera { mat4 viewProj; mat4 prevViewProj; } cam;
layout(set = 1, binding = 5, std430) readonly buffer Draws { DrawRecord draws[]; };

layout(location = 8) flat out uint fragInstance;
layout(location = 9) flat out uint fragMaterialSlot;

void main() {
    // firstInstance = the draw's index in RenderScene's draw table (GPU culling's commands)
    const DrawRecord d = draws[gl_InstanceIndex];
    const uint slot = d.draw.w;
    InstanceGpu inst = instances[slot];

    // World space position
    vec4 worldPos = inst.world * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // Transform normal to world space (assuming uniform scaling)
    mat3 normalMatrix = transpose(inverse(mat3(inst.world)));
    fragNormal = normalize(normalMatrix * inNormal);
    fragTangent = vec4(normalize(normalMatrix * inTangent.xyz), inTangent.w);

    // Pass through vertex color and texture coordinates
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;

    // Current frame clip position
    fragCurrentPos = cam.viewProj * worldPos;

    // Previous frame clip position for velocity calculation. A skinned instance's own mesh keeps
    // last frame's deformed position in boneWeights.xyz (skin/skin.slang).
    const vec3 prevLocal = (d.info.y & kDrawSkinned) != 0u ? inBoneWeights.xyz : inPosition;
    fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(prevLocal, 1.0));

    fragInstance = slot;
    fragMaterialSlot = d.info.x;
    gl_Position = fragCurrentPos;
}
