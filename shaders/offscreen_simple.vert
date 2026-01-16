#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragPos;

// Camera matrices
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} ubo;

// Per-object model transform and material
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    vec2 padding;
} pc;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragPos = worldPos.xyz;

    // Transform normal to world space
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    // Use material base color if vertex color is white/default, otherwise blend
    fragColor = pc.baseColor * inColor;

    gl_Position = ubo.proj * ubo.view * worldPos;
}
