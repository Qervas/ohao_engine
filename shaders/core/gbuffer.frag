#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "includes/scene/scene_data.glsl"
#include "shared/layout.glsl"

// G-Buffer Fragment Shader
// Outputs to multiple render targets for deferred shading. A fragment of a subsurface
// material also flags its 16x16 tile for the SSS pass (postprocess/sss.slang), so depth is
// tested before the shader runs: only visible fragments flag.
layout(early_fragment_tests) in;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec2 fragTexCoord1;
layout(location = 5) in vec4 fragTangent;
layout(location = 6) in vec4 fragCurrentPos;
layout(location = 7) in vec4 fragPrevPos;

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

layout(location = 8) flat in uint fragInstance;

// Set 1 "scene": RenderScene's instance, material and material-reference tables.
layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 1, std430) readonly buffer Materials { vec4 matRows[]; };
layout(set = 1, binding = 2, std430) readonly buffer MaterialRefs { uint materialRefs[]; };
// SSS classification (kSSSHeaderWords header: two indirect dispatches; then the per-tile mask,
// bit 0 skin, bit 1 under the horizontal blur; then the vertical and horizontal tile lists).
layout(set = 1, binding = 4, std430) buffer SSSTiles { uint sssTiles[]; };

// The draw's submesh material slot and the frame's SSS tiles across and down (16 bytes,
// fragment stage).
layout(push_constant) uniform PushConstants { uint materialSlot; uint tilesX; uint tilesY; uint pad; } pc;

// Flags `tile` with `bit` and, the first time, appends it to the list whose count is
// header word `countWord` (the indirect dispatch's x). A plain read first: most fragments of a
// skin tile find it already flagged and take no atomic.
void flagTile(uint tile, uint bit, uint countWord, uint listBase) {
    if ((sssTiles[kSSSHeaderWords + tile] & bit) != 0u) return;
    if ((atomicOr(sssTiles[kSSSHeaderWords + tile], bit) & bit) != 0u) return;
    sssTiles[listBase + atomicAdd(sssTiles[countWord], 1u)] = tile;
}

void flagSkin(uvec2 pixel) {
    const uvec2 t = pixel / kSSSTile;
    const uint vList = kSSSHeaderWords + kSSSMaxTiles;
    const uint hList = vList + kSSSMaxTiles;
    flagTile(t.y * pc.tilesX + t.x, 1u, 0u, vList);
    for (int d = -int(kSSSRadiusTiles); d <= int(kSSSRadiusTiles); ++d) {
        const int row = int(t.y) + d;
        if (row >= 0 && row < int(pc.tilesY)) flagTile(uint(row) * pc.tilesX + t.x, 2u, 4u, hList);
    }
}

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
    InstanceGpu inst = instances[fragInstance];
    uint m = OHAO_MATERIAL_INDEX(inst, pc.materialSlot, materialRefs);
    vec4 row0 = matRows[m * 3u + 0u];   // baseColor, albedoTex
    vec4 row1 = matRows[m * 3u + 1u];   // roughness, metallic, normalTex, emissiveTex
    vec4 row2 = matRows[m * 3u + 2u];   // roughMetalTex, emissiveStrength

    // Normalize interpolated normal
    vec3 N = normalize(fragNormal);

    // Check for bindless albedo texture
    uint albedoTexIdx = floatBitsToUint(row0.w);
    vec3 albedo;
    if (albedoTexIdx != 0xFFFFFFFFu) {
        vec4 texColor = texture(textures[nonuniformEXT(albedoTexIdx)], fragTexCoord);
        albedo = texColor.rgb * fragColor;
    } else {
        // Combine vertex color with material base color
        albedo = fragColor * row0.rgb;
    }

    // Check for bindless normal map
    uint normalTexIdx = floatBitsToUint(row1.z);
    if (normalTexIdx != 0xFFFFFFFFu) {
        vec2 normalUV = fragTexCoord;  // Standardize on UV0 for now, same as RT path
        vec3 tangentNormal = texture(textures[nonuniformEXT(normalTexIdx)], normalUV).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;

        vec3 T;
        vec3 B;
        if (dot(fragTangent.xyz, fragTangent.xyz) > 1e-6) {
            T = normalize(fragTangent.xyz - N * dot(N, fragTangent.xyz));
            B = normalize(cross(N, T)) * fragTangent.w;
        } else {
            vec3 dPdx = dFdx(fragWorldPos);
            vec3 dPdy = dFdy(fragWorldPos);
            vec2 dUVdx = dFdx(normalUV);
            vec2 dUVdy = dFdy(normalUV);
            T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
            B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);
        }
        mat3 TBN = mat3(T, B, N);

        N = normalize(TBN * tangentNormal);
    }

    // Per-pixel roughness/metallic from texture (if available)
    float metallic = row1.y;
    float roughness = row1.x;
    float ao = 1.0;

    uint roughMetalTexIdx = floatBitsToUint(row2.x);
    if (roughMetalTexIdx < 4096u) {
        // GLTF metallicRoughness texture — use UV0 to match RT path and standard models
        vec4 rm = texture(textures[nonuniformEXT(roughMetalTexIdx)], fragTexCoord);
        ao *= rm.r;         // R channel = AO
        roughness *= rm.g;  // G channel = Roughness
        metallic *= rm.b;   // B channel = Metallic
    }

    // Safety: don't allow roughness to be absolute zero (causes mirror artifacts)
    roughness = max(roughness, 0.04);

    // GBuffer0: World Position + Metallic
    outGBuffer0 = vec4(fragWorldPos, metallic);

    // Sample emissive texture if available
    float emissiveLuminance = 0.0;
    uint emissiveTexIdx = floatBitsToUint(row1.w);
    if (emissiveTexIdx < 4096u) {
        vec3 emissiveColor = texture(textures[nonuniformEXT(emissiveTexIdx)], fragTexCoord).rgb;
        emissiveLuminance = dot(emissiveColor, vec3(0.2126, 0.7152, 0.0722)) * row2.y;
    }

    // GBuffer1: Encoded Normal + Roughness + (emissive luminance, or minus the subsurface
    // strength: an emissive surface takes no SSS)
    vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;
    const float subsurface = emissiveLuminance > 0.0 ? 0.0 : row2.z;
    outGBuffer1 = vec4(encodedNormal, roughness, subsurface > 0.0 ? -subsurface : emissiveLuminance);
    if (subsurface > 0.0) flagSkin(uvec2(gl_FragCoord.xy));

    // GBuffer2: Albedo + AO
    outGBuffer2 = vec4(albedo, ao);

    // Velocity: Current - Previous screen position
    // Convert from clip space to NDC ([-1,1])
    vec2 currentNDC = fragCurrentPos.xy / fragCurrentPos.w;
    vec2 prevNDC = fragPrevPos.xy / fragPrevPos.w;

    // Velocity in NDC space (multiply by 0.5 to get screen-space UV difference)
    outVelocity = (currentNDC - prevNDC) * 0.5;
}
