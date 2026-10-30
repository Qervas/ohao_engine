// The transparent pass's blended meshes (sub-project 12, "Materials: alpha modes"): a glTF BLEND
// material's draw, vertex shader core/gbuffer.vert (the draw table through firstInstance), shaded
// per pixel as core/deferred_lighting.frag shades a G-buffer pixel: the clustered lights (the
// forward copy of its loop, includes/lighting/clustered_lights.slang), the sun's shadow by one ray
// query (flag 8), RT GI's radiance or the environment's irradiance or the constant ambient, the
// split-sum environment specular, emission. Colour is blended by coverage (base colour alpha x the
// albedo texture's x opacity); coverage also goes to the transparency mask.
// Set 0: the bindless textures; set 1: GBufferPass's scene set (instances, materials, refs, ...);
// set 2: the forward set (lights, clusters, environment, TLAS, RT GI).
import shared.layout;
import includes.lighting.clustered_lights;
import material.openpbr;

// The bindless table at its size (BindlessTextureManager's 4096): Slang decorates an unsized
// resource array with an ArrayStride that spirv-val rejects (VUID-StandaloneSpirv-None-10684).
[[vk::binding(0, 0)]] Sampler2D textures[4096];
[[vk::binding(0, 1)]] StructuredBuffer<float4> instances;   // InstanceGpu, kInstanceGpuStride / 16 a slot
[[vk::binding(1, 1)]] StructuredBuffer<float4> matRows;     // kMaterialRows a material
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialRefs;
[[vk::binding(0, 2)]] StructuredBuffer<LightGpu> lights;
[[vk::binding(1, 2)]] StructuredBuffer<uint> clusters;
[[vk::binding(2, 2)]] Texture2D<float4> envIrradiance;
[[vk::binding(3, 2)]] Texture2D<float4> envPrefiltered;
[[vk::binding(4, 2)]] SamplerState linearSampler;
[[vk::binding(5, 2)]] RaytracingAccelerationStructure scene;
[[vk::binding(6, 2)]] Texture2D<float4> giRadiance;
[[vk::binding(7, 2)]] StructuredBuffer<float> openpbrTables;   // OpenPBR's energy tables
[[vk::push_constant]] ConstantBuffer<ForwardPush> pc;

struct FsIn
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float3 vertexColor;
    [[vk::location(3)]] float2 uv;
    [[vk::location(4)]] float2 uv1;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float4 currentPos;
    [[vk::location(7)]] float4 prevPos;
    [[vk::location(8)]] nointerpolation uint instance;
    [[vk::location(9)]] nointerpolation uint materialSlot;
};

struct FsOut
{
    [[vk::location(0)]] float4 color;
    [[vk::location(1)]] float coverage;
};

uint materialOf(uint slot, uint materialSlot)
{
    const uint4 info = asuint(instances[slot * (kInstanceGpuStride / 16u) + 8u]);   // mesh, refOffset, refCount, flags
    return materialSlot < info.z ? materialRefs[info.y + materialSlot] : 0u;
}

float sunVisibility(float3 p, float3 n)
{
    if ((pc.config.y & 8u) == 0u) return 1.0;
    for (uint k = 0u; k < pc.config.x; ++k)
    {
        const LightGpu light = lights[k];
        if (int(light.position.w) != 0 || int(round(light.params.w)) <= 0) continue;
        RayDesc ray;
        ray.Origin = p + n * 0.002;
        ray.TMin = 0.001;
        ray.Direction = normalize(-light.direction.xyz);
        ray.TMax = 10000.0;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
        q.TraceRayInline(scene, 0, kRayMaskScene, ray);
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
    }
    return 1.0;
}

[shader("fragment")]
FsOut fragmentMain(FsIn i, bool frontFacing : SV_IsFrontFace)
{
    const uint m = materialOf(i.instance, i.materialSlot);
    const float4 row0 = matRows[m * kMaterialRows + 0u];
    const float4 row1 = matRows[m * kMaterialRows + 1u];
    const float4 row2 = matRows[m * kMaterialRows + 2u];
    const float4 row3 = matRows[m * kMaterialRows + 3u];
    const float4 row4 = matRows[m * kMaterialRows + 4u];   // specular_color, specular_weight
    const float4 row5 = matRows[m * kMaterialRows + 5u];   // specular_ior, base_diffuse_roughness, base_weight, specularTex
    const float4 row6 = matRows[m * kMaterialRows + 6u];   // emission_color, specularColorTex

    float3 N = normalize(i.normal);
    if (!frontFacing) N = -N;   // a pane seen from behind is lit on that side
    const uint albedoTex = asuint(row0.w);
    float4 albedoSample = float4(1.0);
    if (albedoTex != 0xFFFFFFFFu) albedoSample = textures[NonUniformResourceIndex(albedoTex)].Sample(i.uv);
    const float3 albedo = row0.rgb * albedoSample.rgb;
    const float coverage = saturate(row3.w * albedoSample.a * row3.z);

    const uint normalTex = asuint(row1.z);
    if (normalTex != 0xFFFFFFFFu)
    {
        float3 tn = textures[NonUniformResourceIndex(normalTex)].Sample(i.uv).rgb * 2.0 - 1.0;
        float3 T, B;
        if (dot(i.tangent.xyz, i.tangent.xyz) > 1e-6)
        {
            T = normalize(i.tangent.xyz - N * dot(N, i.tangent.xyz));
            B = normalize(cross(N, T)) * i.tangent.w;
        }
        else
        {
            const float3 dPdx = ddx(i.worldPos), dPdy = ddy(i.worldPos);
            const float2 dUVdx = ddx(i.uv), dUVdy = ddy(i.uv);
            T = normalize(dPdx * dUVdy.y - dPdy * dUVdx.y);
            B = normalize(dPdy * dUVdx.x - dPdx * dUVdy.x);
        }
        N = normalize(T * tn.x + B * tn.y + N * tn.z);
    }
    float metallic = row1.y, roughness = row1.x, ao = 1.0;
    const uint rmTex = asuint(row2.x);
    if (rmTex < 4096u)
    {
        const float4 rm = textures[NonUniformResourceIndex(rmTex)].Sample(i.uv);
        roughness *= rm.g;
        metallic *= rm.b;
    }
    const uint occTex = asuint(row2.w);
    if (occTex < 4096u) ao = textures[NonUniformResourceIndex(occTex)].Sample(i.uv).r;
    float3 emissive = row6.rgb * row2.y;
    const uint emTex = asuint(row1.w);
    if (emTex < 4096u) emissive *= textures[NonUniformResourceIndex(emTex)].Sample(i.uv).rgb;
    float specularWeight = row4.w;
    float3 specularColor = row4.rgb;
    const uint specTex = asuint(row5.w);
    if (specTex < 4096u) specularWeight *= textures[NonUniformResourceIndex(specTex)].Sample(i.uv).a;
    const uint specColorTex = asuint(row6.w);
    if (specColorTex < 4096u) specularColor *= textures[NonUniformResourceIndex(specColorTex)].Sample(i.uv).rgb;

    const float3 P = i.worldPos;
    const float3 V = normalize(pc.camera.xyz - P);
    const float viewDepth = -mul(pc.view, float4(P, 1.0)).z;
    MaterialInputs mi;
    mi.baseColor = albedo;
    mi.baseWeight = row5.z;
    mi.baseMetalness = metallic;
    mi.baseDiffuseRoughness = row5.y;
    mi.specularWeight = specularWeight;
    mi.specularColor = specularColor;
    mi.specularRoughness = roughness;
    mi.specularIor = row5.x;
    const Surface surface = makeSurface(P, N, V, mi);
    OpenPBRTables tables;
    tables.data = openpbrTables;
    const bool clustered = (pc.config.y & 1u) != 0u;
    const uint2 list = lightList(clusters, clustered, pc.config.x, i.position.xy, pc.screen.xy, viewDepth, pc.screen.z,
                                 pc.screen.w);
    const float3 Lo = directLight(surface, lights, clusters, list, clustered, directionalVisibility(sunVisibility(P, N)), tables);
    const float3 diffuseAlbedo = surface.bsdf.diffuseAlbedo(surface.wo, tables);
    const float3 specularAlbedo = surface.bsdf.specularAlbedo(surface.wo, tables);

    const bool rtGI = (pc.config.y & 2u) != 0u;
    const bool ibl = (pc.config.y & 4u) != 0u;
    const float4 gi = rtGI ? giRadiance.SampleLevel(linearSampler, i.position.xy / pc.screen.xy, 0.0) : float4(0.0, 0.0, 0.0, 1.0);
    float3 ambient = float3(0.0);
    if (rtGI) ambient += gi.rgb * diffuseAlbedo * ao;
    else if (!ibl) ambient += float3(pc.camera.w) * diffuseAlbedo * ao;
    const float NdotV = surface.wo.z;
    const float3 reflectWeight = specularAlbedo * specularOcclusion(NdotV, ao, roughness);
    if (ibl)
    {
        const float3 R = reflect(-V, N);
        const float3 prefiltered = pc.env.x * envPrefiltered.SampleLevel(linearSampler, equirectUV(R),
                                                                        roughness * float(kEnvPrefilterLevels - 1u)).rgb;
        const float3 irradiance = pc.env.x * envIrradiance.SampleLevel(linearSampler, equirectUV(N), 0.0).rgb;
        const float3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, float3(0.0)) : float3(0.0));
        ambient += incoming * reflectWeight;
        if (!rtGI) ambient += diffuseAlbedo * irradiance * ao;
    }

    FsOut o;
    o.color = float4(ambient + Lo + emissive, coverage);
    o.coverage = coverage;
    return o;
}
