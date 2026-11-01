// The transparent pass's blended meshes (sub-project 12, "Materials: alpha modes"): a glTF BLEND
// material's draw, vertex shader core/gbuffer.slang (the draw table through firstInstance), shaded
// per pixel as core/deferred_lighting.frag shades a G-buffer pixel: the clustered lights (the
// forward copy of its loop, includes/lighting/clustered_lights.slang), the sun's shadow by one ray
// query (flag 8), RT GI's radiance or the environment's irradiance (else no indirect light), the
// split-sum environment specular, emission. Colour is blended by coverage (base colour alpha x the
// albedo texture's x opacity); coverage also goes to the transparency mask.
// Set 0: the bindless textures; set 1: GBufferPass's scene set (instances, materials, refs, ...);
// set 2: the forward set (lights, clusters, environment, TLAS, RT GI).
import shared.layout;
import includes.lighting.clustered_lights;
import material.openpbr;
import material.material_layers;

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
[[vk::binding(8, 2)]] Texture2D<float4> refraction;           // the lit frame before this pass (glass refracts it)
[[vk::binding(9, 2)]] StructuredBuffer<uint4> meshes;          // vertexOffset, indexOffset, indexCount, triangleSlotOffset
[[vk::binding(10, 2)]] StructuredBuffer<uint> indices;         // mesh-local
[[vk::binding(11, 2)]] StructuredBuffer<float> vertices;       // Vertex: kVertexFloats, normal at 6

// Glass's refraction in the deferred frame (OpenPBR step 3, T4): traced. A solid's view ray refracts
// at the front, meets the same instance's far side by a ray query (reflected inside on total internal
// reflection, up to kGlassBounces times), refracts out there by the interpolated normal, and goes on
// to the first opaque surface; the lit frame before this pass is read where that point lands on
// screen (where it is off screen, at the nearest edge pixel; where the ray leaves the scene, the
// environment), spread over a disc as rough refraction spreads it: each interface turns the ray by
// about alpha x (1 - 1/eta) entering and alpha x (eta - 1) leaving, over the distance travelled after
// it, projected at the point's depth (at most kRefractionBlur of the frame's height); tinted, and absorbed by Beer-Lambert over the length travelled inside (transmission_color at
// transmission_depth). A thin wall passes the view ray straight. So a glass ball shows the room
// behind it upside down, as the path tracer does. Light that reflects at the exit is taken to leave
// elsewhere (no loss: a white ball under a white sky stays white). One layer: glass behind glass, or
// an effect drawn after the copy, is not seen through it, and the point read may be hidden on screen
// by something nearer the camera.
static const float kRefractionBlur = 0.15;
static const uint kBlurTaps = 16u;
static const uint kGlassBounces = 4u;
static const uint kVertexFloats = 25u;   // Vertex (rt/alpha_test.slang, the path tracer)
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

float3 vertexAttr(uint v, uint k)
{
    const uint b = v * kVertexFloats + k;
    return float3(vertices[b], vertices[b + 1u], vertices[b + 2u]);
}

// Where a ray from inside instance `slot` meets its surface: 1, with the distance and the
// interpolated normal there turned outward (along d); 2 when an opaque surface inside comes first
// (its distance); 0 when it meets neither (an open mesh).
static const uint kExitNone = 0u;
static const uint kExitSurface = 1u;
static const uint kExitBlocked = 2u;

uint glassExit(uint slot, float3 o, float3 d, out float t, out float3 nOut)
{
    t = 0.0;
    nOut = d;
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-4;
    ray.TMax = 1.0e4;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, kTlasMaskScene | kTlasMaskTransparent, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CandidateInstanceID() == slot)
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return kExitNone;
    t = q.CommittedRayT();
    if (q.CommittedInstanceID() != slot) return kExitBlocked;
    const uint4 mesh = meshes[asuint(instances[slot * (kInstanceGpuStride / 16u) + 8u]).x];
    const uint base = mesh.y + q.CommittedPrimitiveIndex() * 3u;
    const uint v0 = mesh.x + indices[base], v1 = mesh.x + indices[base + 1u], v2 = mesh.x + indices[base + 2u];
    const float2 bc = q.CommittedTriangleBarycentrics();
    const float3x3 w2o = (float3x3)q.CommittedWorldToObject3x4();
    const float3 p0 = vertexAttr(v0, 0u), p1 = vertexAttr(v1, 0u), p2 = vertexAttr(v2, 0u);
    float3 geo = normalize(mul(cross(p1 - p0, p2 - p0), w2o));
    if (dot(geo, d) < 0.0) geo = -geo;
    const float3 nObj = vertexAttr(v0, 6u) * (1.0 - bc.x - bc.y) + vertexAttr(v1, 6u) * bc.x + vertexAttr(v2, 6u) * bc.y;
    float3 n = dot(nObj, nObj) > 1e-8 ? normalize(mul(nObj, w2o)) : geo;
    if (dot(n, geo) < 0.0) n = -n;
    nOut = n;
    return kExitSurface;
}

// The first opaque surface along a ray (alpha-tested and blended instances are passed).
bool opaqueHit(float3 o, float3 d, out float3 p)
{
    p = o;
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-3;
    ray.TMax = 1.0e4;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, kTlasMaskScene, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    p = o + d * q.CommittedRayT();
    return true;
}

struct TransparentTextures : IMaterialTextures
{
    float4 sample(uint index, float2 uv) { return textures[NonUniformResourceIndex(index)].Sample(uv); }
};

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

    // The normal faces the viewer by the rasterised surface (the cross of the world position's
    // screen derivatives), as the G-buffer turns it: the winding, which SV_IsFrontFace reads, is not
    // the same across assets and primitives (the generated sphere's is inverted), so a sphere's front
    // was shaded from behind. A pane seen from behind is lit on that side.
    float3 N = normalize(i.normal);
    if (dot(cross(ddx(i.worldPos), ddy(i.worldPos)), N) > 0.0) N = -N;
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
    MaterialInputs mi = defaultMaterialInputs();
    mi.baseColor = albedo;
    mi.baseWeight = row5.z;
    mi.baseMetalness = metallic;
    mi.baseDiffuseRoughness = row5.y;
    mi.specularWeight = specularWeight;
    mi.specularColor = specularColor;
    mi.specularRoughness = roughness;
    mi.specularIor = row5.x;
    const uint lr = m * kMaterialRows + kLayerRowFirst;
    TransparentTextures tex;
    applyLayerRows(mi, matRows[lr], matRows[lr + 1u], matRows[lr + 2u], matRows[lr + 3u], matRows[lr + 4u], i.uv, tex);
    const uint tr = m * kMaterialRows + kTransmissionRowFirst;
    applyTransmissionRows(mi, matRows[tr], matRows[tr + 1u], matRows[tr + 2u]);
    // A solid's far side (its outward vertex normal turned away from the viewer) is the inside of the
    // glass: the near side's fragment traces through it, as the path tracer's camera ray meets only
    // the near surface. Drawn, it lay over the near side and refracted out of the back.
    if (mi.transmissionWeight > 0.0 && metallic < 1.0 && !mi.thinWalled && dot(i.normal, V) < 0.0) discard;
    applySubsurfaceRows(mi, matRows[m * kMaterialRows + 2u], matRows[m * kMaterialRows + kSubsurfaceRowFirst]);
    float3 tangent = float3(0.0);
    if (mi.specularRoughnessAnisotropy > 0.0 || (mi.coatWeight > 0.0 && mi.coatRoughnessAnisotropy > 0.0))
    {
        const float3 tp = i.tangent.xyz - N * dot(N, i.tangent.xyz);
        float3 b2;
        if (dot(tp, tp) > 1e-12) tangent = normalize(tp);
        else normalBasis(N, tangent, b2);
        tangent = rotatedTangent(N, tangent, matRows[lr + 3u].z);
    }
    const Surface surface = makeSurface(P, N, V, mi, tangent);
    OpenPBRTables tables;
    tables.data = openpbrTables;
    const bool clustered = (pc.config.y & 1u) != 0u;
    const uint2 list = lightList(clusters, clustered, pc.config.x, i.position.xy, pc.screen.xy, viewDepth, pc.screen.z,
                                 pc.screen.w);
    const float3 Lo = directLight(surface, lights, clusters, list, clustered, directionalVisibility(sunVisibility(P, N)), tables);
    const float3 diffuseAlbedo = surface.bsdf.diffuseAlbedo(surface.wo, tables);
    const float3 specularAlbedo = surface.bsdf.specularAlbedo(surface.wo, tables);

    // RT GI's radiance at this pixel is the surface's behind it: glass reflects the environment.
    const bool transmissive = mi.transmissionWeight > 0.0 && metallic < 1.0;
    const bool rtGI = (pc.config.y & 2u) != 0u && !transmissive;
    const bool ibl = (pc.config.y & 4u) != 0u;
    const float4 gi = rtGI ? giRadiance.SampleLevel(linearSampler, i.position.xy / pc.screen.xy, 0.0) : float4(0.0, 0.0, 0.0, 1.0);
    float3 ambient = float3(0.0);
    if (rtGI) ambient += gi.rgb * diffuseAlbedo * ao;
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

    // Transmission: what the surface passes of what the refracted ray meets behind it.
    float3 transmitted = float3(0.0);
    if (transmissive)
    {
        const float4 r12 = matRows[tr], r13 = matRows[tr + 1u];
        const float3 passes = surface.bsdf.transmissionAlbedo(surface.wo, tables);
        float2 uv = i.position.xy / pc.screen.xy;
        float inside = 0.0;
        bool screen = true;
        float3 d = -V;
        const float eta = max(mi.specularIor, 1.0);
        const float alpha = roughness * roughness;
        const float turnIn = alpha * (1.0 - 1.0 / eta), turnOut = alpha * (eta - 1.0);
        float blur = 0.0;   // the spread's radius at the point read, in metres
        float hitW = 1.0;   // that point's view depth (clip w)
        if (mi.thinWalled)
        {
            // Straight through: the two interfaces' turns spread the ray over what lies behind.
            float3 hitP;
            if (turnIn > 0.0 && opaqueHit(P, d, hitP))
            {
                blur = (turnIn + turnOut) * length(hitP - P);
                hitW = mul(pc.proj, mul(pc.view, float4(hitP, 1.0))).w;
            }
        }
        else
        {
            d = refract(-V, N, 1.0 / eta);
            if (!(dot(d, d) > 0.0)) d = -V;
            float3 o = P;
            bool blocked = false;
            for (uint k = 0u; k < kGlassBounces; ++k)
            {
                float t;
                float3 nb;
                const uint e = glassExit(i.instance, o, d, t, nb);
                if (e == kExitNone) break;   // an open mesh: on as it goes
                inside += t;
                o += d * t;
                if (e == kExitBlocked)
                {
                    blocked = true;   // an opaque surface inside the glass: what the ray sees
                    break;
                }
                const float3 out = refract(d, -nb, eta);
                if (dot(out, out) > 0.0)
                {
                    d = normalize(out);
                    break;
                }
                d = reflect(d, -nb);   // total internal reflection
            }
            float3 hitP = o;
            if (blocked || opaqueHit(o, d, hitP))
            {
                const float4 clip = mul(pc.proj, mul(pc.view, float4(hitP, 1.0)));
                if (clip.w > 1e-4) uv = clip.xy / clip.w * 0.5 + 0.5;
                else screen = false;
                const float behindLen = length(hitP - o);
                blur = turnIn * (inside + behindLen) + (blocked ? 0.0 : turnOut * behindLen);
                hitW = clip.w;
            }
            else
            {
                screen = false;
            }
        }
        float3 behind = float3(0.0);
        if (screen || (pc.config.y & 4u) == 0u)
        {
            // The world radius projected: an offset r at view depth w moves uv by r proj[1][1] / (2 w).
            const float radius = min(0.5 * blur * abs(pc.proj[1][1]) / max(hitW, 1e-3), kRefractionBlur);
            [unroll]
            for (uint k = 0u; k < kBlurTaps; ++k)
            {
                const float a = float(k) * 2.39996323;   // the golden angle
                const float r = radius * sqrt((float(k) + 0.5) / float(kBlurTaps));
                const float2 o = float2(cos(a), sin(a)) * r * float2(pc.screen.y / pc.screen.x, 1.0);
                behind += refraction.SampleLevel(linearSampler, saturate(uv + o), 0.0).rgb;
            }
            behind *= 1.0 / float(kBlurTaps);
        }
        else
        {
            // Out of the scene: the environment along the ray, prefiltered by the roughness.
            behind = pc.env.x * envPrefiltered.SampleLevel(linearSampler, equirectUV(d),
                                                           roughness * float(kEnvPrefilterLevels - 1u)).rgb;
        }
        // transmission_color at transmission_depth: Beer-Lambert over the length inside; at depth 0
        // the module's tint already holds the colour.
        const float3 absorbed = r13.x > 0.0 ? pow(max(r12.rgb, float3(1e-4)), float3(inside / r13.x)) : float3(1.0);
        transmitted = passes * absorbed * behind;
    }

    FsOut o;
    o.color = float4(ambient + Lo + emissive + transmitted, coverage);
    o.coverage = coverage;
    return o;
}
