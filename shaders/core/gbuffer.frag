#version 450

// G-Buffer Fragment Shader
// Outputs to multiple render targets for deferred shading

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec4 fragCurrentPos;
layout(location = 5) in vec4 fragPrevPos;

// G-Buffer outputs
// GBuffer0: World Position (rgb) + Metallic (a) - R16G16B16A16_SFLOAT
layout(location = 0) out vec4 outGBuffer0;

// GBuffer1: Encoded Normal (rgb) + Roughness (a) - R16G16B16A16_SFLOAT or R10G10B10A2_UNORM
layout(location = 1) out vec4 outGBuffer1;

// GBuffer2: Albedo (rgb) + AO (a) - R8G8B8A8_SRGB
layout(location = 2) out vec4 outGBuffer2;

// Velocity buffer for TAA - R16G16_SFLOAT
layout(location = 3) out vec2 outVelocity;

// Per-object push constants (matches GBufferUBO in C++)
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 projection;
    mat4 prevMVP;
    vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=unused
    vec4 albedoColor;     // rgb=albedo, a=unused
} pc;

// Normal encoding using octahedron mapping for better precision
vec2 signNotZero(vec2 v) {
    return vec2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? 1.0 : -1.0);
}

vec2 encodeNormalOctahedron(vec3 n) {
    // Project onto octahedron
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    // Fold bottom hemisphere
    return (n.z < 0.0) ? ((1.0 - abs(p.yx)) * signNotZero(p)) : p;
}

void main() {
    // Normalize interpolated normal
    vec3 N = normalize(fragNormal);

    // Combine vertex color with material base color
    vec3 albedo = fragColor * pc.albedoColor.rgb;

    // GBuffer0: World Position + Metallic
    outGBuffer0 = vec4(fragWorldPos, pc.materialParams.x);

    // GBuffer1: Encoded Normal + Roughness
    // Using octahedron encoding for normal, stored in xy
    vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5; // Map to [0,1]
    outGBuffer1 = vec4(encodedNormal, 0.0, pc.materialParams.y);

    // GBuffer2: Albedo + AO
    outGBuffer2 = vec4(albedo, pc.materialParams.z);

    // Velocity: Current - Previous screen position
    // Convert from clip space to NDC ([-1,1])
    vec2 currentNDC = fragCurrentPos.xy / fragCurrentPos.w;
    vec2 prevNDC = fragPrevPos.xy / fragPrevPos.w;

    // Velocity in NDC space (multiply by 0.5 to get screen-space UV difference)
    outVelocity = (currentNDC - prevNDC) * 0.5;
}
