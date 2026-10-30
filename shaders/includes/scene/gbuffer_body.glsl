#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "includes/scene/scene_data.glsl"
#include "shared/layout.glsl"

// G-Buffer fragment shader body (core/gbuffer.frag, core/gbuffer_mask.frag).
// Outputs to multiple render targets for deferred shading. A fragment of a subsurface
// material also flags its 16x16 tile for the SSS pass (postprocess/sss.slang). The opaque
// entry tests depth before the shader runs, so only visible fragments flag; the MASK entry
// (OHAO_GBUFFER_MASK, an alpha-tested material: gpu::shared::kAlphaMask) discards below the
// material's cutoff and so cannot force early tests.
#ifndef OHAO_GBUFFER_MASK
layout(early_fragment_tests) in;
#endif

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

// Emitted radiance - R16G16B16A16_SFLOAT: emission_color times emission_luminance, times the
// emissive texture when there is one (linear: the bindless table's images are sRGB), as the path
// tracer's hit emits.
layout(location = 4) out vec4 outEmissive;

// OpenPBR's specular (RGBA8_UNORM): specular_color (rgb, the KHR specular colour texture applied),
// specular_weight / 1.5 (a, the KHR specular texture's alpha applied): 1, the default, is code 170
// exactly (as weight / 2 it was 127.5, and read back 0.996: a white metal reflected 0.4% less).
layout(location = 5) out vec4 outSpecular;

// OpenPBR's base (RG8_UNORM): base_diffuse_roughness, (specular_ior - 1) / 2.55: steps of 0.01 in
// the IOR, 1.5 (the default) code 50 exactly.
layout(location = 6) out vec2 outOpenPBR;

// Bindless texture array (set 0, binding 0)
layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 8) flat in uint fragInstance;
layout(location = 9) flat in uint fragMaterialSlot;   // the draw record's

// Set 1 "scene": RenderScene's instance, material and material-reference tables.
layout(set = 1, binding = 0, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 1, binding = 1, std430) readonly buffer Materials { vec4 matRows[]; };
layout(set = 1, binding = 2, std430) readonly buffer MaterialRefs { uint materialRefs[]; };
// SSS classification (kSSSHeaderWords header: two indirect dispatches; then the per-tile mask,
// bit 0 skin, bit 1 under the horizontal blur; then the vertical and horizontal tile lists).
layout(set = 1, binding = 4, std430) buffer SSSTiles { uint sssTiles[]; };

// The frame's SSS tiles across and down (16 bytes, fragment stage).
layout(push_constant) uniform PushConstants { uint tilesX; uint tilesY; uint pad0; uint pad1; } pc;

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
    uint m = OHAO_MATERIAL_INDEX(inst, fragMaterialSlot, materialRefs);
    vec4 row0 = matRows[m * kMaterialRows + 0u];   // baseColor, albedoTex
    vec4 row1 = matRows[m * kMaterialRows + 1u];   // roughness, metallic, normalTex, emissiveTex
    vec4 row2 = matRows[m * kMaterialRows + 2u];   // roughMetalTex, emissiveStrength, subsurface, occlusionTex
    vec4 row4 = matRows[m * kMaterialRows + 4u];   // specular_color, specular_weight
    vec4 row5 = matRows[m * kMaterialRows + 5u];   // specular_ior, base_diffuse_roughness, base_weight, specularTex
    vec4 row6 = matRows[m * kMaterialRows + 6u];   // emission_color, specularColorTex

    // Normalize interpolated normal
    vec3 N = normalize(fragNormal);

    // Velocity: Current - Previous screen position
    // Convert from clip space to NDC ([-1,1])
    vec2 currentNDC = fragCurrentPos.xy / fragCurrentPos.w;
    vec2 prevNDC = fragPrevPos.xy / fragPrevPos.w;
    // Velocity in NDC space (multiply by 0.5 to get screen-space UV difference)
    outVelocity = (currentNDC - prevNDC) * 0.5;

    // A light's emitter mesh (kInstanceEmitter): pure emission, its radiance in
    // InstanceGpu.emitterLight.rgb, on the front side (black behind, as the path tracer's
    // hit). Emissive alpha 1 tells lighting to add nothing and SSR to reflect nothing of it:
    // the analytic light already lights and reflects.
    if ((inst.flags & kInstanceEmitter) != 0u) {
        const vec3 radiance = gl_FrontFacing ? inst.emitterLight.rgb : vec3(0.0);
        outGBuffer0 = vec4(fragWorldPos, -1.0);   // metallic -1: RT shadows and GI skip it as sky
        outGBuffer1 = vec4(encodeNormalOctahedron(N) * 0.5 + 0.5, 1.0, dot(radiance, vec3(0.2126, 0.7152, 0.0722)));
        outGBuffer2 = vec4(0.0, 0.0, 0.0, 1.0);
        outEmissive = vec4(radiance, 1.0);
        outSpecular = vec4(0.0);
        outOpenPBR = vec2(0.0);
        return;
    }

    // A double-sided surface seen from behind is shaded on that side, as the path tracer's hit
    // turns its normal toward the ray: the inside of a box, a curtain's back. Without it the normal
    // pointed away and lights and GI's rays met the other side. The side is the rasterised
    // surface's (the cross of the world position's screen derivatives), not the winding, which
    // assets and primitives do not agree on.
    const vec3 geometric = cross(dFdx(fragWorldPos), dFdy(fragWorldPos));
    if (dot(geometric, N) > 0.0) N = -N;

    // Albedo: the material's base colour, times the albedo texture when there is one, as the
    // path tracer's hit computes it (rt/path_tracer.slang's closestHit). The vertex colour is not a
    // factor: primitives carry their colour as vertex colour and base colour both, and
    // multiplying the two squared it.
    uint albedoTexIdx = floatBitsToUint(row0.w);
    vec3 albedo = row0.rgb;
#ifdef OHAO_GBUFFER_MASK
    // Coverage: the base colour's alpha times the albedo texture's times the opacity, against
    // the cutoff (row 3: mode, cutoff, opacity, base alpha), as every ray's candidate test.
    const vec4 row3 = matRows[m * kMaterialRows + 3u];
    vec4 albedoSample = vec4(1.0);
    if (albedoTexIdx != 0xFFFFFFFFu)
        albedoSample = texture(textures[nonuniformEXT(albedoTexIdx)], fragTexCoord);
    if (row3.w * albedoSample.a * row3.z < row3.y) discard;
    albedo *= albedoSample.rgb;
#else
    if (albedoTexIdx != 0xFFFFFFFFu)
        albedo *= texture(textures[nonuniformEXT(albedoTexIdx)], fragTexCoord).rgb;
#endif

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
        roughness *= rm.g;  // G channel = Roughness
        metallic *= rm.b;   // B channel = Metallic
    }
    // Occlusion only from glTF's occlusionTexture (R); a metallicRoughness map's R is unused.
    uint occlusionTexIdx = floatBitsToUint(row2.w);
    if (occlusionTexIdx < 4096u) ao = texture(textures[nonuniformEXT(occlusionTexIdx)], fragTexCoord).r;

    // The roughness as authored: OpenPBR's module holds it at its floor (kMinSpecularRoughness),
    // as the path tracer's does.

    // GBuffer0: World Position + Metallic
    outGBuffer0 = vec4(fragWorldPos, metallic);

    // Emitted radiance: emission_color x emission_luminance, times the emissive texture's colour
    vec3 emissive = row6.rgb * row2.y;
    uint emissiveTexIdx = floatBitsToUint(row1.w);
    if (emissiveTexIdx < 4096u)
        emissive *= texture(textures[nonuniformEXT(emissiveTexIdx)], fragTexCoord).rgb;
    outEmissive = vec4(emissive, 0.0);
    const float emissiveLuminance = dot(emissive, vec3(0.2126, 0.7152, 0.0722));

    // GBuffer1: Encoded Normal + Roughness + (emissive luminance, or minus the subsurface
    // strength: an emissive surface takes no SSS)
    vec2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;
    const float subsurface = emissiveLuminance > 0.0 ? 0.0 : row2.z;
    outGBuffer1 = vec4(encodedNormal, roughness, subsurface > 0.0 ? -subsurface : emissiveLuminance);
    if (subsurface > 0.0) flagSkin(uvec2(gl_FragCoord.xy));

    // GBuffer2: Albedo (base_weight x base_color: OpenPBR's diffuse albedo and a metal's F0) + AO
    outGBuffer2 = vec4(albedo * row5.z, ao);

    // OpenPBR's specular and base inputs (KHR_materials_specular's textures applied)
    float specularWeight = row4.w;
    vec3 specularColor = row4.rgb;
    const uint specularTexIdx = floatBitsToUint(row5.w);
    if (specularTexIdx < 4096u) specularWeight *= texture(textures[nonuniformEXT(specularTexIdx)], fragTexCoord).a;
    const uint specularColorTexIdx = floatBitsToUint(row6.w);
    if (specularColorTexIdx < 4096u) specularColor *= texture(textures[nonuniformEXT(specularColorTexIdx)], fragTexCoord).rgb;
    outSpecular = vec4(clamp(specularColor, 0.0, 1.0), clamp(specularWeight / 1.5, 0.0, 1.0));
    outOpenPBR = vec2(clamp(row5.y, 0.0, 1.0), clamp((row5.x - 1.0) / 2.55, 0.0, 1.0));
}
