#version 450

// G-Buffer Vertex Shader
// Transforms vertices and passes data to fragment shader for G-Buffer population

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) out vec4 fragCurrentPos;  // Current frame clip position
layout(location = 5) out vec4 fragPrevPos;     // Previous frame clip position (for velocity)

// Per-object push constants (matches GBufferUBO in C++)
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 prevMVP;
    vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=unused
    vec4 albedoColor;     // rgb=albedo, a=unused
} pc;

void main() {
    // World space position
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;

    // Transform normal to world space (assuming uniform scaling)
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    // Pass through vertex color and texture coordinates
    fragColor = inColor;
    fragTexCoord = inTexCoord;

    // Current frame clip position
    fragCurrentPos = pc.projection * pc.view * worldPos;

    // Previous frame clip position for velocity calculation
    fragPrevPos = pc.prevMVP * vec4(inPosition, 1.0);

    gl_Position = fragCurrentPos;
}
