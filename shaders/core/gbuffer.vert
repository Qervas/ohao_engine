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

// Set 1 "scene": RenderScene's instance table and the frame slot's camera.
layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 3) uniform Camera { mat4 viewProj; mat4 prevViewProj; } cam;

layout(location = 8) flat out uint fragInstance;

void main() {
    // firstInstance = the RenderScene slot
    InstanceGpu inst = instances[gl_InstanceIndex];

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

    // Previous frame clip position for velocity calculation
    fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(inPosition, 1.0));

    fragInstance = gl_InstanceIndex;
    gl_Position = fragCurrentPos;
}
