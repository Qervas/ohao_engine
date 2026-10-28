// RT shadows' trace (sub-project 2b spec, "RT shadows" and "Reference quality"): a compute
// pass of inline ray queries (no ray tracing pipeline, no shader binding table: a shadow ray
// needs no hit shader). Two qualities, one pass:
//
// realtime   Each pixel picks up to `extent.w` (<= kShadowLightsPerPixel) of the frame's
//            shadow-casting lights by importance (intensity over squared distance, ties by
//            light index) and traces one ray to a point sampled on each light's area. It
//            writes no visibility: `meta` packs the pixel's guide (distance from the camera,
//            normal) with one traced bit and one visible bit a light, which the filter
//            (shadow_filter.slang) denoises spatially and temporally. A pixel with more
//            lights that matter than it may trace counts once in `dropped`.
// reference  Every light, kShadowReferenceRays rays each, no cap, no filter: refLo/refHi keep
//            the plain running mean of the reference frames since the last temporal reset (a
//            realtime frame starts it again; lights 0-3 and 4-7, 32-bit floats) and outVis
//            gets it, 8 bits a light.
//
// Every sample hashes (render seed, frame index since the reset, pixel, light, ray,
// dimension): it changes per frame so the accumulation converges, and repeats exactly after
// a reset. Light l of the buffer is visibility channel l: the lighting pass's params.w.
import shared.layout;
import rt.alpha_test;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] [[vk::image_format("rg32ui")]] RWTexture2D<uint2> meta;
[[vk::binding(2, 0)]] Texture2D<float4> gPosition;   // xyz world position, (0,0,0,0) sky
[[vk::binding(3, 0)]] Texture2D<float4> gNormal;     // xy octahedral normal in [0, 1]
[[vk::binding(4, 0)]] StructuredBuffer<ShadowLight> lights;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> dropped;
[[vk::binding(6, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> refLo;
[[vk::binding(7, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> refHi;
[[vk::binding(8, 0)]] [[vk::image_format("rg32ui")]] RWTexture2D<uint2> outVis;   // reference: what lighting reads
[[vk::push_constant]] ConstantBuffer<ShadowTracePush> pc;

static const float kTwoPi = 6.2831853;

// PCG hash (Jarzynski & Olano 2020): integer in, integer out, no float state.
uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The pixel's hash of (render seed, frame index since the reset, pixel), once per pixel.
uint pixelHash(uint2 pixel)
{
    return pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y))));
}

// Eight 8-bit channels in a uint2, rounded to nearest (shadow_filter.slang reads them).
uint pack4(float4 v)
{
    const uint4 q = uint4(saturate(v) * 255.0 + 0.5);
    return q.x | (q.y << 8u) | (q.z << 16u) | (q.w << 24u);
}

// A uniform number in [0, 1) from the pixel's hash and (light, ray, dimension).
float rand01(uint pixelKey, uint light, uint s, uint dim)
{
    const uint h = pcg(pixelKey ^ pcg((light << 24u) ^ (s << 4u) ^ dim));
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

// What a light is worth to this pixel: intensity over squared distance (a directional
// light: its intensity), 0 out of a point light's range or wholly behind the surface. The
// light's centre and bounding radius come precomputed (ShadowLight): no square root here.
float importance(ShadowLight light, float3 p, float3 n)
{
    const uint type = uint(light.positionAndType.w + 0.5);
    const float intensity = light.centreAndIntensity.w;
    const float radius = light.edge1AndRadius.w;
    if (type == 0u) return dot(n, light.centreAndIntensity.xyz) < -radius ? 0.0 : intensity;
    const float3 d = light.centreAndIntensity.xyz - p;
    const float d2 = dot(d, d);
    const float range = light.edge2AndRange.w + radius;
    if (type == 1u && range > radius && d2 > range * range) return 0.0;
    const float nd = dot(n, d);
    if (nd < 0.0 && nd * nd > radius * radius * d2 && d2 > radius * radius) return 0.0;   // behind the surface, the whole light
    const float cosOuter = light.edge2AndRange.x;   // a spot's outer cone; -2: a point light
    if (type == 1u && cosOuter > -1.5 && d2 > radius * radius)
    {
        // Outside the cone (widened by the light's angular radius) the spot lights nothing.
        const float invDist = rsqrt(d2);
        if (dot(-d * invDist, light.edge1AndRadius.xyz) < cosOuter - radius * invDist) return 0.0;
    }
    return intensity / max(d2, 1e-4);
}

// One ray from `origin` toward a point on light `l` (sample `s`): 1 lit, 0 occluded.
float traceOne(uint pixelKey, uint l, uint s, float3 p, float3 origin, float3 n)
{
    const ShadowLight light = lights[l];
    const uint type = uint(light.positionAndType.w + 0.5);
    const float r1 = rand01(pixelKey, l, s, 0u);
    const float r2 = rand01(pixelKey, l, s, 1u);
    float3 dir;
    float tMax;
    if (type == 0u)
    {
        // Directional: a direction within the light's angular radius
        const float3 toLight = normalize(-light.positionAndType.xyz);
        float3 t, b;
        basis(toLight, t, b);
        const float angle = light.edge1AndRadius.w * sqrt(r1);
        const float phi = kTwoPi * r2;
        dir = normalize(toLight + t * (angle * cos(phi)) + b * (angle * sin(phi)));
        tMax = 10000.0;
    }
    else
    {
        float3 target;
        if (type == 2u)
        {
            target = light.positionAndType.xyz + light.edge1AndRadius.xyz * r1 + light.edge2AndRange.xyz * r2;
        }
        else
        {
            // Uniform on the sphere: z = cos(polar angle), no acos/sin of it.
            const float z = 1.0 - 2.0 * r2;
            const float s = sqrt(max(0.0, 1.0 - z * z));
            const float phi = kTwoPi * r1;
            target = light.positionAndType.xyz + light.edge1AndRadius.w * float3(s * cos(phi), s * sin(phi), z);
        }
        const float3 toLight = target - p;
        tMax = length(toLight);
        dir = toLight / tMax;
    }
    if (dot(n, dir) <= 0.0) return 0.0;   // a surface facing away from the sample gets nothing from it

    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = tMax;
    // Any hit occludes: the first one ends the search. An instance with an alpha-tested or blended
    // material is non-opaque: its candidates occlude where rt/alpha_test.slang says they are hit.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(scene, 0, kRayMaskScene, ray);   // a light's emitter mesh never occludes
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= pc.extent.xy)) return;
    const bool reference = pc.seed.z == 1u;
    const uint count = min(pc.extent.z, kShadowMaxLights);

    const float4 position = gPosition.Load(int3(pixel, 0));
    if (all(position == float4(0.0)) || position.w < 0.0)   // sky, or a light's emitter mesh: nothing to shadow
    {
        if (reference)
        {
            refLo[pixel] = float4(1.0);
            refHi[pixel] = float4(1.0);
            outVis[pixel] = uint2(0xFFFFFFFFu);
        }
        else
        {
            meta[pixel] = uint2(0u, 0u);
        }
        return;
    }
    const float3 p = position.xyz;
    const float2 octN = gNormal.Load(int3(pixel, 0)).xy;
    const float3 n = decodeOctahedron(octN);
    const float3 origin = p + n * 0.05;
    const uint key0 = pixelHash(pixel);

    // Light loops are unrolled over kShadowMaxLights so every per-light value has a constant
    // index (a register, not local memory).
    if (reference)
    {
        // The running mean of the reference frames in a row: with none before (a reset, fresh
        // images, realtime frames in between) this frame takes its own value and nothing stale
        // or undefined is read.
        const float k = float(pc.history.x);
        const float4 prevLo = pc.history.x == 0u ? float4(0.0) : refLo[pixel];
        const float4 prevHi = pc.history.x == 0u ? float4(0.0) : refHi[pixel];
        float4 lo = float4(1.0), hi = float4(1.0);
        [unroll]
        for (uint l = 0u; l < kShadowMaxLights; ++l)
        {
            if (l >= count || importance(lights[l], p, n) <= 0.0) continue;   // contributes nothing here: lit
            float v = 0.0;
            for (uint s = 0u; s < kShadowReferenceRays; ++s) v += traceOne(key0, l, s, p, origin, n);
            v /= float(kShadowReferenceRays);
            if (l < 4u) lo[l] = v; else hi[l - 4u] = v;
        }
        const float4 meanLo = prevLo + (lo - prevLo) / (k + 1.0);
        const float4 meanHi = prevHi + (hi - prevHi) / (k + 1.0);
        refLo[pixel] = meanLo;
        refHi[pixel] = meanHi;
        outVis[pixel] = uint2(pack4(meanLo), pack4(meanHi));
        return;
    }

    // Realtime: the most important lights first, at most extent.w of them. A light's key is
    // its importance with its index in the low 3 bits (a lower index ranks higher on a tie);
    // positive floats order as their bits do. The pixel traces the keys at or above the
    // budget-th largest.
    uint key[kShadowMaxLights];
    uint wanted = 0u;
    [unroll]
    for (uint l = 0u; l < kShadowMaxLights; ++l)
    {
        const float imp = l < count ? importance(lights[l], p, n) : 0.0;
        key[l] = imp > 0.0 ? (asuint(imp) & ~15u) | 8u | (7u - l) : 0u;   // bit 3 set: never 0 when imp > 0
        if (key[l] != 0u) ++wanted;
    }
    const uint budget = min(pc.extent.w, kShadowLightsPerPixel);
    if (wanted > budget) InterlockedAdd(dropped[0], 1u);

    uint threshold = 0xFFFFFFFFu;
    [unroll]
    for (uint k = 0u; k < kShadowLightsPerPixel; ++k)
    {
        if (k >= budget) break;
        uint next = 0u;
        [unroll]
        for (uint l = 0u; l < kShadowMaxLights; ++l)
            if (key[l] < threshold) next = max(next, key[l]);
        threshold = next;
    }
    uint traced = 0u, visible = 0u;
    if (budget > 0u)
    {
        [unroll]
        for (uint l = 0u; l < kShadowMaxLights; ++l)
        {
            if (key[l] == 0u || key[l] < threshold) continue;
            traced |= 1u << l;
            if (traceOne(key0, l, 0u, p, origin, n) > 0.5) visible |= 1u << l;
        }
    }

    // The filter's guide: distance from the camera (bfloat16: a float's top 16 bits, 0.4%
    // precision, and no 16-bit capability) and the normal (8 + 8 bits).
    const uint dist = asuint(distance(p, pc.cameraPos.xyz)) >> 16u;
    const uint2 oct = uint2(saturate(octN) * 255.0 + 0.5);
    meta[pixel] = uint2(dist | (oct.x << 16u) | (oct.y << 24u), traced | (visible << 8u) | (1u << 16u));
}
