// RT GI's trace (sub-project 2b spec, "RT GI" and "Reference quality"): one-bounce diffuse
// indirect light, a compute pass of inline ray queries. Two qualities, one pass:
//
// realtime   Half resolution: trace pixel q shades full-resolution pixel 2q with
//            kGIRealtimeRays cosine rays. It writes the mean incoming radiance (rgb, not
//            multiplied by the receiver's albedo) and the environment's visibility (a, the
//            share of the rays that left the scene), and the denoiser's guide (distance from
//            the camera, normal); gi_denoise.slang accumulates, filters and upsamples them.
// reference  Full resolution, kGIReferenceRays rays a pixel, no denoiser: refMean keeps the
//            plain running mean of the reference frames since the last temporal reset (a
//            realtime frame starts it again), and radianceOut gets it, what lighting reads.
//
// Lighting multiplies the radiance by the diffuse albedo, and lights the specular IBL with the
// environment where the rays escaped and with what they hit elsewhere.
//
// A ray that hits a surface brings back what that surface reflects toward it and its emission. Its
// OpenPBR albedo (its material resolved at the hit's uv, rt/alpha_test.slang's hitMaterial) times
//   the direct light it receives over pi: the frame's lights (every raster light but flashes),
//     attenuated as the lighting pass attenuates them, the hit's normal taken as facing the ray, and
//     shadowed: one light, chosen in proportion to what it would give, gets a shadow ray (directAt).
//     Without it a hit in shadow bounced full light, and every surface facing a shadowed floor (an
//     underside, a rim) glowed;
//   plus the light it receives from everything else (frontier F5a): one cosine ray from the hit
//     brings back what the radiance cache holds where it lands (every bounce beyond) and that
//     surface's emission, or the environment.
// The same reflected light is added to the hit's cell of the cache (rt/radiance_cache.slang, RT GI's
// own: render/rt/radiance_cache.*), whose resolve blends it after the trace; the next frame's second
// rays read it. Each frame carries light one bounce further (SHaRC's propagation by short paths), so
// a still view converges to every bounce. A second ray landing within a voxel of the hit reads no
// cell (a cell would feed itself). A ray that leaves the scene brings back the environment map in its
// direction (black without one). Cosine sampling makes the mean of those radiances the diffuse
// irradiance over pi.
//
// The reflection (frontier F5b): one more ray a pixel, its direction drawn from the GGX lobe of the
// surface's roughness (bounded VNDF, Eto and Tokuyoshi 2023), brings back what it meets as the
// diffuse rays do (a surface's reflected light, the cache's past the first ray, its emission unless
// RT direct light samples it, else the environment). Its mean over the lobe is the radiance the
// split sum prefiltered from the environment, now traced: lighting weighs it by OpenPBR's specular
// albedo. Written beside the diffuse with the roughness in alpha (realtime: specOut, denoised by
// roughness; reference: the running mean in specRefMean).
//
// Every sample hashes (render seed, frame index since the reset, pixel, ray, dimension), so
// the accumulation converges and repeats exactly after a reset.
import shared.layout;
import rt.alpha_test;
import material.openpbr;
import rt.smoke_proxy;
import rt.radiance_cache;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float4> gPosition;   // xyz world position, w metallic; (0,0,0,0) sky
[[vk::binding(2, 0)]] Texture2D<float4> gNormal;     // xy octahedral normal in [0, 1]
[[vk::binding(3, 0)]] Texture2D<float4> gAlbedo;     // rgb albedo
[[vk::binding(4, 0)]] ByteAddressBuffer instances;   // RenderScene's InstanceGpu table, by slot
[[vk::binding(5, 0)]] StructuredBuffer<GILight> lights;
[[vk::binding(6, 0)]] Texture2D<float4> envMap;      // equirectangular, as the lighting pass maps it
[[vk::binding(7, 0)]] SamplerState envSampler;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> radianceOut;
[[vk::binding(9, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> guideOut;     // realtime
[[vk::binding(10, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> refMean; // reference
[[vk::binding(11, 0)]] StructuredBuffer<float> openpbrTables;                        // OpenPBR's energy tables
// The radiance cache (render/rt/radiance_cache.hpp): read through `resolved`, added to through `accum`.
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> cacheKeys;
[[vk::binding(13, 0)]] RWStructuredBuffer<uint4> cacheAccum;
[[vk::binding(14, 0)]] RWStructuredBuffer<uint4> cacheResolved;
// The reflection: realtime the trace's (rgb radiance, a roughness), reference the output it averages into.
[[vk::binding(15, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> specOut;
[[vk::binding(16, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> specRefMean;   // reference
[[vk::binding(17, 0)]] Texture2D<uint2> gLayers;     // OpenPBR's layers (material/material_layers.slang packLayers)
[[vk::push_constant]] ConstantBuffer<GITracePush> pc;

static const float kPi = 3.14159265;
static const float kTwoPi = 6.2831853;
static const float kRayLength = 100.0;
static const uint kReflectionRay = 64u;   // the reflection's ray index for its random numbers (past any diffuse ray's)

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint pixelHash(uint2 pixel)
{
    return pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y))));
}

// A uniform number in [0, 1) from the pixel's hash and (ray, dimension).
float rand01(uint pixelKey, uint s, uint dim)
{
    const uint h = pcg(pixelKey ^ pcg((s << 4u) ^ dim));
    return float(h >> 8u) * (1.0 / 16777216.0);
}

float3 decodeOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void basis(float3 n, out float3 t, out float3 b)
{
    const float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// Whether `p` sees along the unit direction `L` up to `tMax`: one ray that stops at its first
// hit, alpha-tested candidates as the main trace tests them (a cut-out lets light through).
bool visible(float3 p, float3 L, float tMax)
{
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = L;
    ray.TMin = 0.001;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kRayMaskScene, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What `light` gives a bounce point at `p` with normal `n`, unshadowed: the lighting pass's
// attenuation (inverse square windowed by the range, a spot's cone) and the cosine. `L` and `tMax`
// are the direction to it and how far a shadow ray goes.
float3 lightAt(GILight light, float3 p, float3 n, out float3 L, out float tMax)
{
    const uint type = uint(light.positionAndType.w + 0.5);
    float atten = 1.0;
    tMax = kRayLength;
    if (type == 0u)
    {
        L = -light.directionAndRange.xyz;
    }
    else
    {
        const float3 d = light.positionAndType.xyz - p;
        const float d2 = max(dot(d, d), 1e-8);
        const float dist = sqrt(d2);
        L = d / dist;
        tMax = dist;
        const float range = light.directionAndRange.w;
        const float x = range > 0.0 ? dist / range : 0.0;
        const float window = saturate(1.0 - x * x * x * x);
        atten = window * window / max(d2, 0.01);
        if (type == 2u)
            atten *= saturate((dot(L, -light.directionAndRange.xyz) - light.cone.y) / max(light.cone.x - light.cone.y, 1e-4));
    }
    return light.radiance.xyz * (atten * max(dot(n, L), 0.0));
}

// A light drawn in proportion to its power from the alias table (Vose): `u` picks a slot and its
// remainder keeps it or takes its alias. Returns the light and its probability.
uint drawByPower(float u, out float pdf)
{
    const uint count = pc.seed.w;
    const float x = u * float(count);
    const uint slot = min(uint(x), count - 1u);
    const float4 entry = lights[slot].cone;
    const uint pick = (x - float(slot)) < entry.z ? slot : asuint(entry.w);
    pdf = lights[pick].radiance.w;
    return pick;
}

// The direct light a bounce point at `p` with normal `n` receives from every light, only where
// `p` sees the light, at the cost of one shadow ray: one light is chosen with probability
// proportional to the luminance it would give unshadowed, and its light divided by that
// probability. Up to kGIWeighedLights lights are each weighed; past that, kGILightCandidates
// candidates are drawn by power and one chosen among them by what it would give (resampled importance
// sampling: the estimate is unbiased for any number of lights). Random dimensions: 2 the choice,
// 3 seeds the candidates' sequence.
float3 directAt(float3 p, float3 n, uint key, uint s)
{
    const uint count = pc.seed.w;
    if (count == 0u) return float3(0.0);
    float3 L, lit;
    float tMax, scale;
    if (count <= kGIWeighedLights)
    {
        // One pass, a weighted reservoir: light l replaces the choice with probability w_l over the
        // total so far, and the one number u is rescaled to stay uniform for the next test.
        float u = rand01(key, s, 2u);
        float total = 0.0, w = 0.0;
        for (uint l = 0u; l < count; ++l)
        {
            float3 Ll;
            float tl;
            const float3 c = lightAt(lights[l], p, n, Ll, tl);
            const float wl = luminance(c);
            if (!(wl > 0.0)) continue;
            total += wl;
            // The first light that gives any always takes the choice (wl / wl may round below 1,
            // and u may lie above it), as does a share that rounds to 1 (so 1 - share never
            // divides); u stays below 1 through the rescaling's rounding.
            const float share = wl / total;
            if (w == 0.0 || u < share || share >= 1.0)
            {
                lit = c;
                L = Ll;
                tMax = tl;
                w = wl;
                u = min(u / share, 0.99999994);
            }
            else
            {
                u = min((u - share) / (1.0 - share), 0.99999994);
            }
        }
        if (!(total > 0.0)) return float3(0.0);
        scale = total / w;
    }
    else
    {
        const uint stream = pcg(key ^ pcg((s << 4u) ^ 3u));   // the candidates' own sequence
        uint pick[kGILightCandidates];
        float ris[kGILightCandidates];
        float sum = 0.0;
        [unroll]
        for (uint i = 0u; i < kGILightCandidates; ++i)
        {
            float pdf;
            pick[i] = drawByPower(float(pcg(stream + i) >> 8u) * (1.0 / 16777216.0), pdf);
            float3 Li;
            float ti;
            ris[i] = pdf > 0.0 ? luminance(lightAt(lights[pick[i]], p, n, Li, ti)) / pdf : 0.0;
            sum += ris[i];
        }
        if (!(sum > 0.0)) return float3(0.0);
        float target = rand01(key, s, 2u) * sum;
        uint chosen = 0u;
        [unroll]
        for (uint i = 0u; i < kGILightCandidates; ++i)
        {
            if (ris[i] > 0.0)
            {
                chosen = i;
                if (target < ris[i]) break;
                target -= ris[i];
            }
        }
        lit = lightAt(lights[pick[chosen]], p, n, L, tMax);
        scale = sum / (float(kGILightCandidates) * luminance(lit));
    }
    if (!visible(p + n * 0.01, L, tMax)) return float3(0.0);
    return lit * scale;
}

// The environment's radiance in direction `dir` (0 without one).
float3 environment(float3 dir)
{
    if ((pc.flags.y & 1u) == 0u) return float3(0.0);
    // The environment is uploaded with one mip level (gpu/vulkan/env_upload.cpp): a miss reads
    // full-resolution texels.
    const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / kPi);   // row 0 the zenith
    return envMap.SampleLevel(envSampler, uv, 0.0).rgb * pc.cameraPos.w;   // times Environment.intensity
}

// The closest surface along a ray (alpha-tested, light emitters left out), committed in `q`.
bool closestHit(inout RayQuery<RAY_FLAG_NONE> q, float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = kRayLength;
    q.TraceRayInline(scene, RAY_FLAG_NONE, kRayMaskScene, ray);   // light emitters: camera and mirror rays only
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The light reaching surface `at` (geometric normal ng, facing the ray that found it) from everything
// but the lights: one cosine ray, the cache's radiance and the emission where it lands, else the
// environment.
float3 indirectAt(float3 at, float3 ng, uint key, uint s)
{
    float3 t, b;
    basis(ng, t, b);
    const float u = rand01(key, s, 5u);
    const float phi = kTwoPi * rand01(key, s, 6u);
    const float r = sqrt(u);
    const float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + ng * sqrt(max(0.0, 1.0 - u)));
    const float3 origin = at + ng * 0.05;
    RayQuery<RAY_FLAG_NONE> q;
    if (!closestHit(q, origin, dir)) return environment(dir);
    const float3 hit = origin + dir * q.CommittedRayT();
    const HitMaterial hm = hitMaterial(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                       q.CommittedTriangleBarycentrics());
    float3 n = hitGeometricNormal(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(), q.CommittedObjectToWorld3x4());
    if (dot(n, dir) > 0.0) n = -n;
    float3 radiance = hm.emission;
    const float voxel = cacheVoxelSizeAt(hit, pc.cameraPos.xyz);
    if (q.CommittedRayT() >= voxel)
    {
        // Jittered within the voxel's face (Binder 2019), so no voxel shows as a block.
        float3 jt, jb;
        basis(n, jt, jb);
        const float2 j = (float2(rand01(key, s, 7u), rand01(key, s, 8u)) - 0.5) * voxel;
        float3 cached;
        if (cacheLookup(cacheKeys, cacheResolved, cacheCell(hit + jt * j.x + jb * j.y, n, pc.cameraPos.xyz), cached))
            radiance += cached;
    }
    return radiance;
}

// A pixel nearer the camera than this reads the cache but adds nothing to it: the first-person view
// model is drawn 0.108-0.27 m from the eye (ecs/view_model.hpp), remapped, so its rays start where no
// world surface is, and the player's capsule (0.3 m) keeps the world farther. Its samples would also
// make the rest of the frame depend on whether the gun is drawn.
static const float kCacheFeedNear = 0.3;

// The radiance arriving at `origin` from direction `dir`; `escaped` is 1 when the ray left the
// scene (it then brings back the environment). `feeds`: the hit's reflected light goes into the cache.
float3 incoming(float3 origin, float3 dir, uint key, uint s, bool feeds, out float escaped)
{
    escaped = 0.0;
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = kRayLength;
    // The closest hit; an alpha-tested or blended instance's candidates count where
    // rt/alpha_test.slang says they are hit (a blended surface bounces nothing).
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, kRayMaskScene, ray);   // light emitters: camera and mirror rays only
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    // Smoke (rt/smoke_proxy.slang) on the ray up to what it hit: the radiance behind it is attenuated
    // by its transmittance T and nothing is added (OPEN ruling 29: no in-scatter in 12).
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float tEnd = hit ? q.CommittedRayT() : kRayLength;
    const float T = smokeTransmittance(origin, dir, tEnd);
    if (hit)
    {
        // The hit surface's material at its uv: what it reflects of the light it receives, by its
        // OpenPBR directional albedo toward the ray (its normal taken as facing the ray), and what
        // it emits.
        const HitMaterial hm = hitMaterial(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(),
                                           q.CommittedTriangleBarycentrics());
        OpenPBRTables tables;
        tables.data = openpbrTables;
        // What the hit reflects (a glass's refracted share passes on, it is not reflected light).
        const OpenPBRSurface hs = OpenPBRSurface(hm.inputs);
        const float3 albedo = hs.specularAlbedo(float3(0.0, 0.0, 1.0), tables) + hs.diffuseAlbedo(float3(0.0, 0.0, 1.0), tables);
        const float3 at = origin + dir * q.CommittedRayT();
        float3 ng = hitGeometricNormal(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(), q.CommittedObjectToWorld3x4());
        if (dot(ng, dir) > 0.0) ng = -ng;
        // What the hit reflects. A pixel's first ray computes it (its direct light, and everything
        // else's through the cache) and adds it to the hit's cell; its other rays read their hit's
        // cell, a voxel away or more, and compute it only where the cell has nothing yet.
        float3 reflected;
        bool cached = false;
        if (s > 0u && q.CommittedRayT() >= cacheVoxelSizeAt(at, pc.cameraPos.xyz))
            cached = cacheLookup(cacheKeys, cacheResolved, cacheCell(at, ng, pc.cameraPos.xyz), reflected);
        if (!cached)
        {
            reflected = albedo * ((1.0 / kPi) * directAt(at, ng, key, s) + indirectAt(at, ng, key, s));
            const uint slot = feeds ? cacheInsert(cacheKeys, cacheCell(at, ng, pc.cameraPos.xyz)) : ~0u;
            if (slot != ~0u)
            {
                cacheAdd(cacheAccum, slot, reflected);
                cacheCount(cacheAccum, slot);
            }
        }
        // What the hit emits is direct light when RT direct light samples emissive triangles (flags.y 2).
        const float3 emitted = (pc.flags.y & 2u) != 0u ? float3(0.0) : hm.emission;
        return (reflected + emitted) * T;
    }
    escaped = 1.0;
    return environment(dir) * T;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= pc.extent.xy)) return;
    const bool reference = pc.flags.x == 1u;
    // Realtime traces the frame >> flags.w (1: half resolution, 0: full).
    const uint2 full = min(reference ? pixel : pixel << pc.flags.w, pc.extent.zw - 1u);

    const float4 position = gPosition.Load(int3(full, 0));
    if (all(position == float4(0.0)) || position.w < 0.0)   // sky, or a light's emitter mesh: no indirect light
    {
        radianceOut[pixel] = float4(0.0);
        specOut[pixel] = float4(0.0);
        if (reference)
        {
            refMean[pixel] = float4(0.0);
            specRefMean[pixel] = float4(0.0);
        }
        else guideOut[pixel] = 0u;
        return;
    }
    const float3 p = position.xyz;
    const float2 octN = gNormal.Load(int3(full, 0)).xy;
    const float3 n = decodeOctahedron(octN);
    const float3 origin = p + n * 0.05;
    float3 t, b;
    basis(n, t, b);

    // Cosine-distributed directions, stratified in azimuth across the pixel's rays.
    const uint key = pixelHash(pixel);
    const uint rays = max(pc.seed.z, 1u);
    const bool feeds = distance(p, pc.cameraPos.xyz) >= kCacheFeedNear;
    float3 sum = float3(0.0);
    float escapedSum = 0.0;
    for (uint s = 0u; s < rays; ++s)
    {
        const float u = rand01(key, s, 0u);
        const float phi = kTwoPi * (float(s) + rand01(key, s, 1u)) / float(rays);
        const float r = sqrt(u);
        const float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)));
        float escaped;
        sum += incoming(origin, dir, key, s, feeds, escaped);
        escapedSum += escaped;
    }
    // The mean incoming radiance, and the environment's visibility: the share of the
    // cosine-weighted rays that left the scene.
    const float4 mean = float4(sum / float(rays), escapedSum / float(rays));

    // The reflection: a direction from the GGX lobe toward the viewer (one below the horizon, which
    // the bounded VNDF rarely draws, is mirrored).
    // Under a coat the ray follows the coat's lobe, the sharp one the eye reads (ruling O8); the base's
    // specular beneath reuses its radiance.
    const uint2 layers = gLayers.Load(int3(full, 0));
    const float rough = (layers.y & 255u) != 0u ? clamp(float((layers.y >> 8u) & 255u) / 255.0, 0.01, 1.0)
                                                 : clamp(gNormal.Load(int3(full, 0)).b, 0.01, 1.0);
    const float3 V = normalize(pc.cameraPos.xyz - p);
    const float3 wo = normalize(float3(dot(V, t), dot(V, b), max(dot(V, n), 1e-4)));
    const float3 m = ggxSampleBounded(wo, rough * rough, float2(rand01(key, kReflectionRay, 0u), rand01(key, kReflectionRay, 1u)));
    float3 wi = reflect(-wo, m);
    if (wi.z <= 0.0) wi = float3(-wo.x, -wo.y, wo.z);
    float specEscaped;
    const float3 reflection = incoming(origin, normalize(t * wi.x + b * wi.y + n * wi.z), key, kReflectionRay, feeds,
                                       specEscaped);

    if (reference)
    {
        // The running mean of the reference frames in a row: with none before (a reset, a
        // fresh image, realtime frames in between) this frame takes its own value and nothing
        // stale or undefined is read.
        const uint k = pc.flags.z;
        const float4 prev = k == 0u ? float4(0.0) : refMean[pixel];
        const float4 rm = prev + (mean - prev) / (float(k) + 1.0);
        refMean[pixel] = rm;
        radianceOut[pixel] = rm;
        const float4 sp = k == 0u ? float4(0.0) : specRefMean[pixel];
        const float4 sm = sp + (float4(reflection, rough) - sp) / (float(k) + 1.0);
        specRefMean[pixel] = sm;
        specOut[pixel] = sm;
        return;
    }

    radianceOut[pixel] = mean;
    specOut[pixel] = float4(reflection, rough);
    // The denoiser's guide, as RT shadows pack theirs: the distance from the camera
    // (bfloat16, never 0 on a surface) and the octahedral normal (8 + 8 bits). 0 is sky.
    const uint dist = max(asuint(distance(p, pc.cameraPos.xyz)) >> 16u, 1u);
    const uint2 oct = uint2(saturate(octN) * 255.0 + 0.5);
    guideOut[pixel] = dist | (oct.x << 16u) | (oct.y << 24u);
}
