// The G-buffer: the instance path's vertex shader (also the transparent pass's meshes,
// vfx/transparent_mesh.slang) and its two fragment shaders, `opaque` (early depth tests, so only
// visible fragments flag SSS tiles) and `masked` (glTF MASK, gpu::shared::kAlphaMask: a discard
// below the material's cutoff, so no forced early tests; GBufferPass draws the kDrawMask draws
// with it after the late phase). The meshlet path's vertices come from the same transform
// (includes/scene/gbuffer_vertex.slang, cull/meshlets.slang).
//
// The targets (GBufferPass::GBUFFER_COUNT 8, depth attachment 4):
//   0  world position, metallic (-1: a light's emitter mesh)              R16G16B16A16_SFLOAT
//   1  octahedral normal, roughness, emissive luminance or -subsurface    R16G16B16A16_SFLOAT
//   2  albedo (base_weight x base_color), the occlusion texture           R8G8B8A8_SRGB
//   3  velocity: this frame's NDC less last frame's, x 0.5 (TAA's uv)       R16G16_SFLOAT
//   4  emitted radiance: emission_color x emission_luminance x the emissive texture (linear; the
//      path tracer's hit emits the same)                                  R16G16B16A16_SFLOAT
//   5  OpenPBR's specular_color, specular_weight / 1.5 (1, the default, is code 170 exactly)
//                                                                         R8G8B8A8_UNORM
//   6  OpenPBR's base_diffuse_roughness, (specular_ior - 1) / 2.55 (1.5 is code 50 exactly)
//                                                                         R8G8_UNORM
//   7  OpenPBR's layers (material/material_layers.slang packLayers): the material's index, the
//      anisotropy and its tangent's angle; the coat's and the fuzz's weight and roughness
//                                                                         R32G32_UINT
// A fragment of a subsurface material flags its 16x16 tile for the SSS pass (postprocess/sss.slang).
import shared.layout;
import includes.scene.scene_data;
import includes.scene.gbuffer_vertex;
import material.openpbr;
import material.material_layers;

// Set 0: the bindless textures (at their size: an unsized array gets an ArrayStride spirv-val rejects).
[[vk::binding(0, 0)]] Sampler2D textures[4096];

// Set 1 "scene" (GBufferPass): RenderScene's instance, material and material-reference tables, the
// frame slot's camera, the SSS classification, the draw table.
[[vk::binding(0, 1)]] StructuredBuffer<InstanceGpu> instances;
[[vk::binding(1, 1)]] StructuredBuffer<float4> matRows;
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialRefs;
struct CameraData
{
    float4x4 viewProj;
    float4x4 prevViewProj;
};
[[vk::binding(3, 1)]] ConstantBuffer<CameraData> camera;
// SSS classification (kSSSHeaderWords header: two indirect dispatches; then the per-tile mask,
// bit 0 skin, bit 1 under the horizontal blur; then the vertical and horizontal tile lists).
[[vk::binding(4, 1)]] RWStructuredBuffer<uint> sssTiles;
[[vk::binding(5, 1)]] StructuredBuffer<DrawRecord> draws;

// The frame's SSS tiles across and down (16 bytes, fragment stage).
struct GBufferPush
{
    uint tilesX;
    uint tilesY;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<GBufferPush> pc;

// ---- vertex ------------------------------------------------------------------------------------

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
    [[vk::location(7)]] float4 boneWeights : BLENDWEIGHT;
};

[shader("vertex")]
GVertex vertexMain(VertexIn i, uint instanceIndex : SV_VulkanInstanceID)
{
    // firstInstance = the draw's index in RenderScene's draw table (GPU culling's commands).
    const DrawRecord d = draws[instanceIndex];
    const InstanceGpu inst = instances[d.draw.w];
    // A skinned instance's own mesh keeps last frame's deformed position in boneWeights.xyz
    // (skin/skin.slang).
    const float3 prevLocal = (d.info.y & kDrawSkinned) != 0u ? i.boneWeights.xyz : i.position;
    return transformVertex(instanceXform(inst.world, inst.prevWorld), camera.viewProj, camera.prevViewProj, i.position,
                           i.color, i.normal, i.tangent, i.texCoord, i.texCoord1, prevLocal, d.draw.w, d.info.x);
}

// ---- fragment ----------------------------------------------------------------------------------

struct GBufferOut
{
    float4 gbuffer0 : SV_Target0;
    float4 gbuffer1 : SV_Target1;
    float4 gbuffer2 : SV_Target2;
    float2 velocity : SV_Target3;
    float4 emissive : SV_Target4;
    float4 specular : SV_Target5;
    float2 openPBR : SV_Target6;
    uint2 layers : SV_Target7;
};

// Flags `tile` with `bit` and, the first time, appends it to the list whose count is header word
// `countWord` (the indirect dispatch's x). A plain read first: most fragments of a skin tile find
// it already flagged and take no atomic.
void flagTile(uint tile, uint bit, uint countWord, uint listBase)
{
    if ((sssTiles[kSSSHeaderWords + tile] & bit) != 0u) return;
    uint before;
    InterlockedOr(sssTiles[kSSSHeaderWords + tile], bit, before);
    if ((before & bit) != 0u) return;
    uint at;
    InterlockedAdd(sssTiles[countWord], 1u, at);
    sssTiles[listBase + at] = tile;
}

void flagSkin(uint2 pixel)
{
    const uint2 t = pixel / kSSSTile;
    const uint vList = kSSSHeaderWords + kSSSMaxTiles;
    const uint hList = vList + kSSSMaxTiles;
    flagTile(t.y * pc.tilesX + t.x, 1u, 0u, vList);
    for (int d = -int(kSSSRadiusTiles); d <= int(kSSSRadiusTiles); ++d)
    {
        const int row = int(t.y) + d;
        if (row >= 0 && row < int(pc.tilesY)) flagTile(uint(row) * pc.tilesX + t.x, 2u, 4u, hList);
    }
}

float2 signNotZero(float2 v)
{
    return float2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? 1.0 : -1.0);
}

// The octahedral encoding of a unit normal.
float2 encodeNormalOctahedron(float3 n)
{
    const float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return (n.z < 0.0) ? ((1.0 - abs(p.yx)) * signNotZero(p)) : p;
}

float4 sampleTexture(uint index, float2 uv)
{
    return textures[NonUniformResourceIndex(index)].Sample(uv);
}

struct GBufferTextures : IMaterialTextures
{
    float4 sample(uint index, float2 uv) { return sampleTexture(index, uv); }
};

// The octahedral decode lighting applies to target 1 (includes/lighting/gbuffer_material.slang).
float3 decodeOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// The material's G-buffer values at this fragment; `masked` discards below the cutoff.
GBufferOut shade(GVertex v, bool frontFacing, float4 fragCoord, bool masked)
{
    GBufferOut o;
    const InstanceGpu inst = instances[v.instance];
    const uint m = v.materialSlot < inst.materialRefCount ? materialRefs[inst.materialRefOffset + v.materialSlot] : 0u;
    const float4 row0 = matRows[m * kMaterialRows + 0u];   // baseColor, albedoTex
    const float4 row1 = matRows[m * kMaterialRows + 1u];   // roughness, metallic, normalTex, emissiveTex
    const float4 row2 = matRows[m * kMaterialRows + 2u];   // roughMetalTex, emissiveStrength, subsurface, occlusionTex
    const float4 row4 = matRows[m * kMaterialRows + 4u];   // specular_color, specular_weight
    const float4 row5 = matRows[m * kMaterialRows + 5u];   // specular_ior, base_diffuse_roughness, base_weight, specularTex
    const float4 row6 = matRows[m * kMaterialRows + 6u];   // emission_color, specularColorTex

    float3 N = normalize(v.normal);

    // Velocity: this frame's NDC less last frame's, times 0.5 (a uv difference).
    const float2 currentNDC = v.currentPos.xy / v.currentPos.w;
    const float2 prevNDC = v.prevPos.xy / v.prevPos.w;
    o.velocity = (currentNDC - prevNDC) * 0.5;

    // A light's emitter mesh (kInstanceEmitter): pure emission, its radiance in
    // InstanceGpu.emitterLight.rgb, on the front side (black behind, as the path tracer's hit).
    // Emissive alpha 1 tells lighting to add nothing: the analytic light already lights and reflects.
    if ((inst.flags & kInstanceEmitter) != 0u)
    {
        const float3 radiance = frontFacing ? inst.emitterLight.rgb : float3(0.0);
        o.gbuffer0 = float4(v.worldPos, -1.0);   // metallic -1: RT direct light and GI skip it as sky
        o.gbuffer1 = float4(encodeNormalOctahedron(N) * 0.5 + 0.5, 1.0, dot(radiance, float3(0.2126, 0.7152, 0.0722)));
        o.gbuffer2 = float4(0.0, 0.0, 0.0, 1.0);
        o.emissive = float4(radiance, 1.0);
        o.specular = float4(0.0);
        o.openPBR = float2(0.0);
        return o;
    }

    // A double-sided surface seen from behind is shaded on that side, as the path tracer's hit
    // turns its normal toward the ray: the inside of a box, a curtain's back. The side is the
    // rasterised surface's (the cross of the world position's screen derivatives), not the winding,
    // which assets and primitives do not agree on.
    const float3 geometric = cross(ddx(v.worldPos), ddy(v.worldPos));
    if (dot(geometric, N) > 0.0) N = -N;

    // Albedo: the base colour, times the albedo texture when there is one, as the path tracer's
    // hit computes it. The vertex colour is not a factor: primitives carry their colour as vertex
    // colour and base colour both.
    const uint albedoTexIdx = asuint(row0.w);
    float3 albedo = row0.rgb;
    if (masked)
    {
        // Coverage: the base colour's alpha times the albedo texture's times the opacity, against
        // the cutoff (row 3: mode, cutoff, opacity, base alpha), as every ray's candidate test.
        const float4 row3 = matRows[m * kMaterialRows + 3u];
        float4 albedoSample = float4(1.0);
        if (albedoTexIdx != 0xFFFFFFFFu) albedoSample = sampleTexture(albedoTexIdx, v.texCoord);
        if (row3.w * albedoSample.a * row3.z < row3.y) discard;
        albedo *= albedoSample.rgb;
    }
    else if (albedoTexIdx != 0xFFFFFFFFu)
        albedo *= sampleTexture(albedoTexIdx, v.texCoord).rgb;

    // The normal map (uv 0, as the path tracer reads it), in the interpolated tangent frame, else
    // one from the screen derivatives.
    const uint normalTexIdx = asuint(row1.z);
    if (normalTexIdx != 0xFFFFFFFFu)
    {
        const float2 normalUV = v.texCoord;
        float3 tangentNormal = sampleTexture(normalTexIdx, normalUV).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        float3 T;
        float3 B;
        if (dot(v.tangent.xyz, v.tangent.xyz) > 1e-6)
        {
            T = normalize(v.tangent.xyz - N * dot(N, v.tangent.xyz));
            B = normalize(cross(N, T)) * v.tangent.w;
        }
        else
        {
            const float3 dPdx = ddx(v.worldPos);
            const float3 dPdy = ddy(v.worldPos);
            const float2 dUVdx = ddx(normalUV);
            const float2 dUVdy = ddy(normalUV);
            T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
            B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);
        }
        // T, B, N as the columns (GLSL's mat3(T, B, N) * tangentNormal).
        N = normalize(mul(tangentNormal, float3x3(T, B, N)));
    }

    // Roughness and metallic, times glTF's metallicRoughness texture (G, B; uv 0); occlusion only
    // from its occlusionTexture (R): a metallicRoughness map's R is unused. The roughness as
    // authored: OpenPBR's module holds it at its floor, as the path tracer's does.
    float metallic = row1.y;
    float roughness = row1.x;
    float ao = 1.0;
    const uint roughMetalTexIdx = asuint(row2.x);
    if (roughMetalTexIdx < 4096u)
    {
        const float4 rm = sampleTexture(roughMetalTexIdx, v.texCoord);
        roughness *= rm.g;
        metallic *= rm.b;
    }
    const uint occlusionTexIdx = asuint(row2.w);
    if (occlusionTexIdx < 4096u) ao = sampleTexture(occlusionTexIdx, v.texCoord).r;

    o.gbuffer0 = float4(v.worldPos, metallic);

    float3 emissive = row6.rgb * row2.y;
    const uint emissiveTexIdx = asuint(row1.w);
    if (emissiveTexIdx < 4096u) emissive *= sampleTexture(emissiveTexIdx, v.texCoord).rgb;
    o.emissive = float4(emissive, 0.0);
    const float emissiveLuminance = dot(emissive, float3(0.2126, 0.7152, 0.0722));

    // The normal, the roughness, and the emissive luminance or minus the subsurface strength (an
    // emissive surface takes no SSS).
    const float2 encodedNormal = encodeNormalOctahedron(N) * 0.5 + 0.5;
    const float subsurface = emissiveLuminance > 0.0 ? 0.0 : row2.z;
    o.gbuffer1 = float4(encodedNormal, roughness, subsurface > 0.0 ? -subsurface : emissiveLuminance);
    if (subsurface > 0.0) flagSkin(uint2(fragCoord.xy));

    o.gbuffer2 = float4(albedo * row5.z, ao);

    // OpenPBR's specular and base inputs (KHR_materials_specular's textures applied).
    float specularWeight = row4.w;
    float3 specularColor = row4.rgb;
    const uint specularTexIdx = asuint(row5.w);
    if (specularTexIdx < 4096u) specularWeight *= sampleTexture(specularTexIdx, v.texCoord).a;
    const uint specularColorTexIdx = asuint(row6.w);
    if (specularColorTexIdx < 4096u) specularColor *= sampleTexture(specularColorTexIdx, v.texCoord).rgb;
    o.specular = float4(clamp(specularColor, 0.0, 1.0), clamp(specularWeight / 1.5, 0.0, 1.0));
    o.openPBR = float2(clamp(row5.y, 0.0, 1.0), clamp((row5.x - 1.0) / 2.55, 0.0, 1.0));

    // OpenPBR's layers. The tangent's angle is taken in the basis of the normal lighting decodes
    // (target 1 holds it at half precision), so both read the same direction.
    const uint lr = m * kMaterialRows + kLayerRowFirst;
    MaterialInputs layers = defaultMaterialInputs();
    GBufferTextures tex;
    applyLayerRows(layers, matRows[lr], matRows[lr + 1u], matRows[lr + 2u], matRows[lr + 3u], matRows[lr + 4u],
                   v.texCoord, tex);
    const float3 Nd = decodeOctahedron(encodedNormal);
    float3 T = float3(0.0);
    if (layers.specularRoughnessAnisotropy > 0.0 || (layers.coatWeight > 0.0 && layers.coatRoughnessAnisotropy > 0.0))
    {
        const float3 tp = v.tangent.xyz - Nd * dot(Nd, v.tangent.xyz);
        if (dot(tp, tp) > 1e-12) T = normalize(tp);
        else
        {
            float3 b2;
            normalBasis(Nd, T, b2);
        }
        T = rotatedTangent(Nd, T, matRows[lr + 3u].z);
    }
    o.layers = packLayers(m, layers, Nd, T);
    return o;
}

[shader("fragment")]
[earlydepthstencil]
GBufferOut opaque(GVertex v, bool frontFacing : SV_IsFrontFace)
{
    return shade(v, frontFacing, v.position, false);
}

[shader("fragment")]
GBufferOut masked(GVertex v, bool frontFacing : SV_IsFrontFace)
{
    return shade(v, frontFacing, v.position, true);
}
