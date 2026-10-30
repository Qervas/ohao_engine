// The deferred frame's lighting (OpenPBR step 1, T4): every pixel of the G-buffer shaded through
// OpenPBR's base (material/openpbr.slang), the BSDF the reference path tracer samples, by the
// clustered light loop the forward passes share (includes/lighting/clustered_lights.slang).
//
//   direct      RT direct light (rt/restir_di.slang: every light but flashes, resampled and
//               denoised), its diffuse and specular times their directional albedos, and the
//               froxel's flashes; without it each light of the pixel's froxel, a directional light
//               shadowed by the CSM;
//   indirect    diffuse: RT GI's incoming radiance, else the environment's irradiance, else the
//               constant ambient, times the diffuse's directional albedo beneath the specular layer
//               (EON's, scaled by what the specular lets through); specular: the split sum, the
//               prefiltered environment (and GI's radiance where the rays hit) times the specular
//               lobes' own directional albedo (OpenPBR's energy tables, not a fitted DFG);
//   emission    the G-buffer's emitted radiance, unlit.
//
// Beside the lit image it writes the environment/GI specular it added (E) and its weight (W), which
// SSR's resolve replaces where its trace hits (postprocess/ssr_resolve.slang).
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
[[vk::binding(10, 0)]] Sampler2D ssaoTexture;       // half resolution: (ao, distance from the camera)
[[vk::binding(11, 0)]] Sampler2D ssgiTexture;       // RT GI: mean incoming radiance, the environment's visibility
[[vk::binding(12, 0)]] ConstantBuffer<CascadeData> cascades;
[[vk::binding(14, 0)]] Sampler2D directDiffuse;    // RT direct light's diffuse, over the diffuse albedo
[[vk::binding(21, 0)]] Sampler2D directSpecular;   // and its specular, over the specular albedo
[[vk::binding(15, 0)]] Sampler2D envPrefiltered;    // GGX-prefiltered by roughness, kEnvPrefilterLevels levels
[[vk::binding(16, 0)]] StructuredBuffer<uint> clusters;
[[vk::binding(17, 0)]] Sampler2D gBufferEmissive;   // emitted radiance; a 1 for a light's emitter mesh
[[vk::binding(18, 0)]] Sampler2D gBufferSpecular;   // specular_color, specular_weight / 1.5
[[vk::binding(19, 0)]] Sampler2D gBufferOpenPBR;    // base_diffuse_roughness, (specular_ior - 1) / 2.55
[[vk::binding(20, 0)]] StructuredBuffer<float> openpbrTables;

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
    float ambientIntensity;
    float2 screenSize;
    uint lightCount;   // every light (without clusters)
    uint flags;        // 1 IBL, 2 SSAO, 4 CSM, 8 RT GI, 32 RT direct light, 64 every light (no froxel lists)
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
    float4 envSpecular : SV_Target1;      // E: the environment/GI specular added
    float4 specularWeight : SV_Target2;   // W: its weight (split sum x specular occlusion)
};

float4 mulColumns(float4 c[4], float4 v) { return c[0] * v.x + c[1] * v.y + c[2] * v.z + c[3] * v.w; }

// SSAO at this pixel: the 4 nearest half-resolution samples, weighted bilinearly and by how close
// their distance from the camera is to this pixel's (within 5%); where none is the same surface,
// the closest one.
float ssaoAt(int2 pixel, float dist)
{
    uint w, h, levels;
    ssaoTexture.GetDimensions(0, w, h, levels);
    const int2 size = int2(w, h);
    const int2 base = pixel >> 1;
    const float2 f = float2(pixel & 1) * 0.5;
    float sum = 0.0, wsum = 0.0, best = 1.0, bestErr = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        const int2 o = int2(i & 1, i >> 1);
        const float2 s = ssaoTexture.Load(int3(min(base + o, size - 1), 0)).xy;
        if (s.y <= 0.0) continue;
        const float err = abs(s.y - dist);
        if (err < bestErr) { bestErr = err; best = s.x; }
        const float wt = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * max(0.0, 1.0 - err / (0.05 * dist));
        sum += wt * s.x;
        wsum += wt;
    }
    return wsum > 1e-3 ? sum / wsum : best;
}

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
    o.envSpecular = float4(0.0);
    o.specularWeight = float4(0.0);
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

    // A light's emitter mesh (emissive alpha 1): its radiance alone; with E = W = 0 SSR leaves it.
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
    const MaterialInputs mi = gbufferMaterial(g0, g1, g2, spec, pbr, N, weather);
    const float roughness = mi.specularRoughness;
    const float3 V = normalize(pc.cameraPos - P);
    const Surface s = makeSurface(P, N, V, mi);
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

    // SSAO darkens the ambient and environment terms only (not direct light, which shadows occlude,
    // nor RT GI, whose rays see occluders).
    const float ssao = (pc.flags & 2u) != 0u ? ssaoAt(int2(fragCoord.xy), distance(P, pc.cameraPos)) : 1.0;

    // The surface's directional albedos toward the viewer: what its specular lobes reflect of a
    // uniform surround, and what its diffuse beneath them does.
    const float3 diffuseAlbedo = s.bsdf.diffuseAlbedo(s.wo, tables);
    const float3 specularAlbedo = s.bsdf.specularAlbedo(s.wo, tables);
    if (vis.traced)
        Lo += directDiffuse.SampleLevel(uv, 0.0).rgb * diffuseAlbedo + directSpecular.SampleLevel(uv, 0.0).rgb * specularAlbedo;

    // Indirect diffuse: RT GI's rays (the incoming radiance), else the environment's irradiance
    // (below), else the constant ambient, a fill with no counterpart in the path tracer.
    const bool rtGI = (pc.flags & 8u) != 0u;
    const bool ibl = (pc.flags & 1u) != 0u;
    const float4 gi = rtGI ? ssgiTexture.SampleLevel(uv, 0.0) : float4(0.0, 0.0, 0.0, 1.0);
    float3 ambient = float3(0.0);
    if (rtGI) ambient += gi.rgb * diffuseAlbedo * ao;
    else if (!ibl) ambient += pc.ambientIntensity * diffuseAlbedo * ao * ssao;

    // The reflection's weight, here and for SSR's resolve: the specular albedo times the share of the
    // reflection that survives the occlusion RT GI's rays do not resolve (ao, SSAO) through specular
    // occlusion (the reflection is a narrower cone than the hemisphere they measure).
    const float NdotV = s.wo.z;
    const float occlusion = ao * ssao;
    const float3 specularWeight = specularAlbedo * specularOcclusion(NdotV, occlusion, roughness);
    float3 envSpecular = float3(0.0);
    if (ibl)
    {
        const float3 R = reflect(-V, s.normal);
        const float3 prefiltered = pc.envIntensity *
            envPrefiltered.SampleLevel(equirectUV(R), roughness * float(kEnvPrefilterLevels - 1u)).rgb;
        const float3 irradiance = pc.envIntensity * envIrradiance.SampleLevel(equirectUV(s.normal), 0.0).rgb;   // over pi
        // What arrives: the environment where it is visible and, with RT GI, what its rays that hit
        // something brought back (the incoming radiance less the escaped share's environment) for
        // the rest. Without RT GI the environment is taken as unoccluded.
        const float3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, float3(0.0)) : float3(0.0));
        envSpecular = incoming * specularWeight;
        ambient += envSpecular;
        // RT GI's escaped rays bring the environment back, occluded: without them, its irradiance.
        if (!rtGI) ambient += diffuseAlbedo * irradiance * occlusion;
    }

    o.color = float4(ambient + Lo + emitted.rgb, 1.0);
    o.envSpecular = float4(envSpecular, 0.0);
    o.specularWeight = float4(specularWeight, 0.0);
    return o;
}
