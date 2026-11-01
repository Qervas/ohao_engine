// The deferred frame's lighting (OpenPBR step 1, T4): every pixel of the G-buffer shaded through
// OpenPBR's base (material/openpbr.slang), the BSDF the reference path tracer samples, by the
// clustered light loop the forward passes share (includes/lighting/clustered_lights.slang).
//
//   direct      RT direct light (rt/restir_di.slang: every light but flashes, resampled and
//               denoised), its diffuse and specular times their directional albedos, and the
//               froxel's flashes; without it each light of the pixel's froxel, a directional light
//               shadowed by the CSM;
//   indirect    diffuse: RT GI's incoming radiance, else the environment's irradiance, else none
//               (as in the path tracer), times the diffuse's directional albedo beneath the specular layer
//               (EON's, scaled by what the specular lets through); specular: RT GI's reflection (one
//               ray a pixel from the GGX lobe, frontier F5b), else the prefiltered environment (the
//               split sum), times the specular lobes' own directional albedo (OpenPBR's energy
//               tables, not a fitted DFG);
//   emission    the G-buffer's emitted radiance, unlit.
import shared.layout;
import material.openpbr;
import includes.lighting.clustered_lights;
import includes.lighting.gbuffer_material;

[[vk::binding(0, 0)]] Sampler2D gBuffer0;           // position, metallic
[[vk::binding(1, 0)]] Sampler2D gBuffer1;           // octahedral normal, roughness, emissive luminance / -subsurface
[[vk::binding(2, 0)]] Sampler2D gBuffer2;           // albedo (base_weight x base_color), AO
[[vk::binding(5, 0)]] StructuredBuffer<LightGpu> lights;
[[vk::binding(6, 0)]] Sampler2DArrayShadow shadowMap;   // CSM, comparison LESS_OR_EQUAL
[[vk::binding(7, 0)]] Sampler2D envIrradiance;      // the environment's irradiance over pi
[[vk::binding(11, 0)]] Sampler2D ssgiTexture;       // RT GI: mean incoming radiance, the environment's visibility
[[vk::binding(22, 0)]] Sampler2D giReflection;      // RT GI's reflection: the radiance over the GGX lobe (frontier F5b)
[[vk::binding(12, 0)]] ConstantBuffer<CascadeData> cascades;
[[vk::binding(14, 0)]] Sampler2D directDiffuse;    // RT direct light's diffuse, over the diffuse albedo
[[vk::binding(21, 0)]] Sampler2D directSpecular;   // and its specular, over the specular albedo
[[vk::binding(15, 0)]] Sampler2D envPrefiltered;    // GGX-prefiltered by roughness, kEnvPrefilterLevels levels
[[vk::binding(16, 0)]] StructuredBuffer<uint> clusters;
[[vk::binding(17, 0)]] Sampler2D gBufferEmissive;   // emitted radiance; a 1 for a light's emitter mesh
[[vk::binding(18, 0)]] Sampler2D gBufferSpecular;   // specular_color, specular_weight / 1.5
[[vk::binding(19, 0)]] Sampler2D gBufferOpenPBR;    // base_diffuse_roughness, (specular_ior - 1) / 2.55
[[vk::binding(20, 0)]] StructuredBuffer<float> openpbrTables;
[[vk::binding(23, 0)]] Sampler2D<uint2> gBufferLayers;  // OpenPBR's layers (material/material_layers.slang packLayers)
[[vk::binding(24, 0)]] StructuredBuffer<float4> layerMaterials;   // the material table: the layers' rows

struct CascadeData
{
    float4 viewProj[16];   // four matrices, as their columns
    float4 splitDepths;    // positive view-space distances a cascade ends at
    float cascadeBlendWidth;
    float shadowBias;
    float normalBias;
    uint cascadeCount;
};

// LightingParams (184 bytes): matrices as their columns.
struct LightingPush
{
    float4 invViewProj[4];
    float4 view[4];
    float3 cameraPos;
    float pad0;
    float2 screenSize;
    uint lightCount;   // every light (without clusters)
    uint flags;        // 1 IBL, 4 CSM, 8 RT GI, 32 RT direct light, 64 every light (no froxel lists)
    float wetness;
    float sliceScale;
    float snowCover;
    float sliceBias;
    float frostCover;
    float envIntensity;
};
[[vk::push_constant]] ConstantBuffer<LightingPush> pc;

struct LightingOut
{
    float4 color : SV_Target0;
    // A subsurface pixel's diffuse irradiance and albedo for the SSS pass (packSubsurfaceDiffuse),
    // which spreads the irradiance alone and multiplies it by the pixel's own albedo: the specular
    // stays sharp, and a uniform surround stays uniform. 0 elsewhere.
    uint4 subsurfaceDiffuse : SV_Target1;
};

float4 mulColumns(float4 c[4], float4 v) { return c[0] * v.x + c[1] * v.y + c[2] * v.z + c[3] * v.w; }

int selectCascade(float viewDepth)
{
    const uint count = min(cascades.cascadeCount, 4u);
    for (int i = 0; i < int(count); ++i)
        if (viewDepth < cascades.splitDepths[i]) return i;
    return int(count) - 1;
}

// 3x3 PCF in the pixel's cascade, each tap a hardware comparison.
float shadowCSM(float3 p, float viewDepth)
{
    const int c = selectCascade(viewDepth);
    float4 m[4];
    for (int k = 0; k < 4; ++k) m[k] = cascades.viewProj[c * 4 + k];
    const float4 ls = mulColumns(m, float4(p, 1.0));
    float3 proj = ls.xyz / ls.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    if (any(proj < 0.0) || any(proj > 1.0)) return 1.0;
    uint w, h, layers, levels;
    shadowMap.GetDimensions(0, w, h, layers, levels);
    const float2 texel = 1.0 / float2(w, h);
    const float reference = proj.z - cascades.shadowBias;
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            shadow += shadowMap.SampleCmpLevelZero(float3(proj.xy + float2(x, y) * texel, float(c)), reference);
    return shadow / 9.0;
}

[shader("fragment")]
LightingOut main(float2 uv : TEXCOORD0, float4 fragCoord : SV_Position)
{
    LightingOut o;
    o.subsurfaceDiffuse = uint4(0u);
    const float4 g0 = gBuffer0.SampleLevel(uv, 0.0);
    const float4 g1 = gBuffer1.SampleLevel(uv, 0.0);
    const float4 g2 = gBuffer2.SampleLevel(uv, 0.0);

    // The sky: with an environment (IBL) the environment along the view ray (prefiltered level 0
    // is the source), as the path tracer's camera rays that miss see it; without one the sky pass.
    if (all(g0 == float4(0.0)))
    {
        o.color = float4(0.0);
        if ((pc.flags & 1u) != 0u)
        {
            const float4 far = mulColumns(pc.invViewProj, float4(uv * 2.0 - 1.0, 1.0, 1.0));
            const float3 viewDir = normalize(far.xyz / far.w - pc.cameraPos);
            o.color = float4(pc.envIntensity * envPrefiltered.SampleLevel(equirectUV(viewDir), 0.0).rgb, 1.0);
        }
        return o;
    }

    // A light's emitter mesh (emissive alpha 1): its radiance alone.
    const float4 emitted = gBufferEmissive.SampleLevel(uv, 0.0);
    if (emitted.a > 0.5)
    {
        o.color = float4(emitted.rgb, 1.0);
        return o;
    }

    const float3 P = g0.rgb;
    const float3 N = decodeNormalOctahedron(g1.xy);
    const float ao = g2.a;
    const float4 spec = gBufferSpecular.SampleLevel(uv, 0.0);
    const float2 pbr = gBufferOpenPBR.SampleLevel(uv, 0.0).rg;
    Weather weather;
    weather.wetness = pc.wetness;
    weather.snowCover = pc.snowCover;
    weather.frostCover = pc.frostCover;
    MaterialInputs mi = gbufferMaterial(g0, g1, g2, spec, pbr, N, weather);
    const float3 tangent = gbufferLayers(mi, gBufferLayers.Load(int3(int2(fragCoord.xy), 0)), layerMaterials, N);
    const float roughness = mi.specularRoughness;
    const float3 V = normalize(pc.cameraPos - P);
    const Surface s = makeSurface(P, N, V, mi, tangent);
    OpenPBRTables tables;
    tables.data = openpbrTables;

    // Direct light: the pixel's froxel's lights (in light order), or with flag 64 every light.
    const float viewDepth = -mulColumns(pc.view, float4(P, 1.0)).z;
    const bool clustered = (pc.flags & 64u) == 0u;
    const uint2 list = lightList(clusters, clustered, pc.lightCount, fragCoord.xy, pc.screenSize, viewDepth,
                                 pc.sliceScale, pc.sliceBias);
    LightVisibility vis;
    vis.traced = (pc.flags & 32u) != 0u;
    vis.directional = !vis.traced && (pc.flags & 4u) != 0u ? shadowCSM(P, viewDepth) : 1.0;
    float3 Lo = directLight(s, lights, clusters, list, clustered, vis, tables);

    // The surface's directional albedos toward the viewer: what its specular lobes reflect of a
    // uniform surround, and what its diffuse beneath them does.
    const float3 diffuseAlbedo = s.bsdf.diffuseAlbedo(s.wo, tables);
    const float3 specularAlbedo = s.bsdf.specularAlbedo(s.wo, tables);
    if (vis.traced)
        Lo += directDiffuse.SampleLevel(uv, 0.0).rgb * diffuseAlbedo + directSpecular.SampleLevel(uv, 0.0).rgb * specularAlbedo;

    // Indirect diffuse: RT GI's rays (the incoming radiance), else the environment's irradiance
    // (below), else none: without either a frame has no indirect light, as in the path tracer.
    const bool rtGI = (pc.flags & 8u) != 0u;
    const bool ibl = (pc.flags & 1u) != 0u;
    const float4 gi = rtGI ? ssgiTexture.SampleLevel(uv, 0.0) : float4(0.0, 0.0, 0.0, 1.0);
    float3 ambient = float3(0.0);
    if (rtGI) ambient += gi.rgb * diffuseAlbedo * ao;

    // The reflection's weight: the specular albedo times the share of the reflection that survives
    // the material's AO through specular occlusion (the reflection is a narrower cone than the
    // hemisphere AO measures).
    const float NdotV = s.wo.z;
    const float3 specularWeight = specularAlbedo * specularOcclusion(NdotV, ao, roughness);
    if (rtGI)
    {
        // RT GI's reflection ray: what the lobe sees, traced (the environment where it escapes).
        const float3 reflection = giReflection.SampleLevel(uv, 0.0).rgb;
        const float coatShare = s.bsdf.coatAlbedo(s.wo, tables);
        if (coatShare > 0.0)
        {
            // Under a coat the ray follows the coat's lobe (ruling O8). The base's own specular is
            // rougher: it takes the incoming radiance RT GI's cosine rays measured, toward the coat's
            // ray as the two roughnesses meet (a rough metal under a smooth coat showed the coat's
            // mirror image in its own colour).
            const float3 coatWeight = coatShare * specularOcclusion(NdotV, ao, mi.coatRoughness);
            const float3 baseWeight = max(specularWeight - coatShare * specularOcclusion(NdotV, ao, roughness), float3(0.0));
            const float3 baseIncoming = lerp(reflection, gi.rgb, saturate((roughness - mi.coatRoughness) * 2.0));
            ambient += reflection * coatWeight + baseIncoming * baseWeight;
        }
        else
        {
            ambient += reflection * specularWeight;
        }
    }
    else if (ibl)
    {
        const float3 R = reflect(-V, s.normal);
        const float3 prefiltered = pc.envIntensity *
            envPrefiltered.SampleLevel(equirectUV(R), roughness * float(kEnvPrefilterLevels - 1u)).rgb;
        const float3 irradiance = pc.envIntensity * envIrradiance.SampleLevel(equirectUV(s.normal), 0.0).rgb;   // over pi
        // Without ray tracing the split sum: the environment, unoccluded but by the material's AO;
        // a coat's share at its own roughness.
        float3 reflected = prefiltered * specularWeight;
        const float coatShare = s.bsdf.coatAlbedo(s.wo, tables);
        if (coatShare > 0.0)
        {
            const float coatRough = mi.coatRoughness;
            const float3 coatPrefiltered = pc.envIntensity *
                envPrefiltered.SampleLevel(equirectUV(R), coatRough * float(kEnvPrefilterLevels - 1u)).rgb;
            const float3 coatWeight = coatShare * specularOcclusion(NdotV, ao, coatRough);
            reflected = prefiltered * max(specularWeight - coatShare * specularOcclusion(NdotV, ao, roughness), float3(0.0))
                      + coatPrefiltered * coatWeight;
        }
        ambient += reflected + diffuseAlbedo * irradiance * ao;
    }

    if (g1.w < 0.0)
    {
        float3 d = directLightDiffuse(s, lights, clusters, list, clustered, vis, tables);
        if (vis.traced) d += directDiffuse.SampleLevel(uv, 0.0).rgb * diffuseAlbedo;
        if (rtGI) d += gi.rgb * diffuseAlbedo * ao;
        else if (ibl) d += diffuseAlbedo * pc.envIntensity * envIrradiance.SampleLevel(equirectUV(s.normal), 0.0).rgb * ao;
        o.subsurfaceDiffuse = packSubsurfaceDiffuse(d, diffuseAlbedo);
    }

    o.color = float4(ambient + Lo + emitted.rgb, 1.0);
    return o;
}
