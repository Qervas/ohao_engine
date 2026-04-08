#version 450
#extension GL_EXT_nonuniform_qualifier : require

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

// Bindless texture array (set 0, binding 0)
layout(set = 0, binding = 0) uniform sampler2D textures[];

// Per-object push constants (matches GBufferUBO in C++)
// Total: 224 bytes (3 mat4 + 2 vec4) — fits within 256-byte NVIDIA limit
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;        // precomputed projection * view
    mat4 prevMVP;
    vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=albedoTexIdx (uint bits)
    vec4 albedoColor;     // rgb=albedo, a=normalTexIdx (uint bits)
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

    // Check for bindless albedo texture
    uint albedoTexIdx = floatBitsToUint(pc.materialParams.w);
    vec3 albedo;
    if (albedoTexIdx != 0xFFFFFFFFu) {
        vec4 texColor = texture(textures[nonuniformEXT(albedoTexIdx)], fragTexCoord);
        albedo = texColor.rgb * fragColor;
    } else {
        // Combine vertex color with material base color
        albedo = fragColor * pc.albedoColor.rgb;
    }

    // Check for bindless normal map
    uint normalTexIdx = floatBitsToUint(pc.albedoColor.a);
    if (normalTexIdx != 0xFFFFFFFFu) {
        // Sample normal map and perturb geometric normal
        vec3 tangentNormal = texture(textures[nonuniformEXT(normalTexIdx)], fragTexCoord).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;

        // Compute TBN from derivatives (no explicit tangent attribute needed)
        vec3 dPdx = dFdx(fragWorldPos);
        vec3 dPdy = dFdy(fragWorldPos);
        vec2 dUVdx = dFdx(fragTexCoord);
        vec2 dUVdy = dFdy(fragTexCoord);

        vec3 T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
        vec3 B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);
        mat3 TBN = mat3(T, B, N);

        N = normalize(TBN * tangentNormal);
    }

    // GBuffer0: World Position + Metallic
    outGBuffer0 = vec4(fragWorldPos, pc.materialParams.x);

    // GBuffer1: Encoded Normal + Roughness
    // Using octahedron encoding for normal, stored in xy
    // Roughness in B (10 bits in A2R10G10B10 format), NOT A (only 2 bits)
    vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5; // Map to [0,1]
    outGBuffer1 = vec4(encodedNormal, pc.materialParams.y, 0.0);

    // GBuffer2: Albedo + AO
    outGBuffer2 = vec4(albedo, pc.materialParams.z);

    // Velocity: Current - Previous screen position
    // Convert from clip space to NDC ([-1,1])
    vec2 currentNDC = fragCurrentPos.xy / fragCurrentPos.w;
    vec2 prevNDC = fragPrevPos.xy / fragPrevPos.w;

    // Velocity in NDC space (multiply by 0.5 to get screen-space UV difference)
    outVelocity = (currentNDC - prevNDC) * 0.5;
}
