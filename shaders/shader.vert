#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec3 fragColor;
layout(location = 4) out vec3 fragBaseColor;
layout(location = 5) out float fragMetallic;
layout(location = 6) out float fragRoughness;
layout(location = 7) out float fragAo;

// Push constant for model matrix and material properties
layout(push_constant) uniform PushConstantData {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} pc;

void main() {
    // Use push constant for model matrix instead of UBO
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    // Ensure proper normal transformation
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    fragTexCoord = inTexCoord;
    fragColor = inColor;

    // Pass material properties to fragment shader
    fragBaseColor = pc.baseColor;
    fragMetallic = pc.metallic;
    fragRoughness = pc.roughness;
    fragAo = pc.ao;

    gl_Position = ubo.proj * ubo.view * worldPos;
}
