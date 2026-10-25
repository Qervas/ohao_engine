// RT GI's trace (sub-project 2b spec, "RT GI" and "Reference quality"): one-bounce diffuse
// indirect light, a compute pass of inline ray queries. Two qualities, one pass:
//
// realtime   Half resolution: trace pixel q shades full-resolution pixel 2q with
//            kGIRealtimeRays cosine rays. It writes the mean incoming radiance (not yet
//            multiplied by the receiver's albedo) and the denoiser's guide (distance from the
//            camera, normal); gi_denoise.slang accumulates, filters and upsamples it.
// reference  Full resolution, kGIReferenceRays rays a pixel, no denoiser: frame k after a
//            temporal reset keeps the plain running mean of frames 0..k in refMean and writes
//            mean x diffuse albedo, what lighting adds, to radianceOut.
//
// A ray that hits a surface brings back that surface's bounce albedo (the instance table's
// giAlbedoPacked.rgb) over pi, times the direct light the hit receives from the frame's
// brightest lights (kGIMaxLights), attenuated as the lighting pass attenuates them, with
// no shadow ray and the hit's normal taken as facing the ray. A ray that leaves the scene
// brings back the environment map in its direction (black without one). Cosine sampling
// makes the mean of those radiances the diffuse irradiance over pi.
//
// Every sample hashes (render seed, frame index since the reset, pixel, ray, dimension), so
// the accumulation converges and repeats exactly after a reset.
import shared.layout;

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
[[vk::push_constant]] ConstantBuffer<GITracePush> pc;

static const float kPi = 3.14159265;
static const float kTwoPi = 6.2831853;
static const uint kRayMaskStatic = 0x01u;   // RT_RAY_MASK_STATIC (render/rt/rt_visibility.hpp)
static const float kRayLength = 100.0;
static const float kEnvLod = 4.0;           // a miss reads a blurred mip: less variance a ray

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

// The direct light a bounce point at `p` with normal `n` receives (no shadow ray), the
// lighting pass's attenuation: inverse square windowed by the range, a spot's cone.
float3 directAt(float3 p, float3 n)
{
    float3 sum = float3(0.0);
    [unroll]
    for (uint l = 0u; l < kGIMaxLights; ++l)
    {
        if (l >= pc.seed.w) break;
        const GILight light = lights[l];
        const uint type = uint(light.positionAndType.w + 0.5);
        float3 L;
        float atten = 1.0;
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
            const float range = light.directionAndRange.w;
            const float x = range > 0.0 ? dist / range : 0.0;
            const float window = saturate(1.0 - x * x * x * x);
            atten = window * window / max(d2, 0.01);
            if (type == 2u)
                atten *= saturate((dot(L, -light.directionAndRange.xyz) - light.cone.y) / max(light.cone.x - light.cone.y, 1e-4));
        }
        sum += light.radiance.xyz * (atten * max(dot(n, L), 0.0));
    }
    return sum;
}

// The radiance arriving at `origin` from direction `dir`.
float3 incoming(float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = kRayLength;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, kRayMaskStatic, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const uint slot = q.CommittedInstanceID();   // the instance's custom index: its table slot
        const float3 albedo = asfloat(instances.Load3(slot * kInstanceGpuStride + kInstanceGpuGiAlbedoOffset));
        const float3 hit = origin + dir * q.CommittedRayT();
        return albedo * (1.0 / kPi) * directAt(hit, -dir);
    }
    if (pc.flags.y == 0u) return float3(0.0);
    const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, asin(clamp(dir.y, -1.0, 1.0)) / kPi + 0.5);
    return envMap.SampleLevel(envSampler, uv, kEnvLod).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= pc.extent.xy)) return;
    const bool reference = pc.flags.x == 1u;
    const uint2 full = min(reference ? pixel : pixel * 2u, pc.extent.zw - 1u);

    const float4 position = gPosition.Load(int3(full, 0));
    if (all(position == float4(0.0)))   // sky: no indirect light
    {
        radianceOut[pixel] = float4(0.0);
        if (reference) refMean[pixel] = float4(0.0);
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
    float3 sum = float3(0.0);
    for (uint s = 0u; s < rays; ++s)
    {
        const float u = rand01(key, s, 0u);
        const float phi = kTwoPi * (float(s) + rand01(key, s, 1u)) / float(rays);
        const float r = sqrt(u);
        const float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)));
        sum += incoming(origin, dir);
    }
    const float3 mean = sum / float(rays);

    if (reference)
    {
        // The running mean of frames 0..k: frame 0 takes its own value, so nothing before
        // the reset (or in a fresh image) is read.
        const float3 prev = pc.seed.y == 0u ? float3(0.0) : refMean[pixel].xyz;
        const float3 m = prev + (mean - prev) / (float(pc.seed.y) + 1.0);
        refMean[pixel] = float4(m, 1.0);
        const float3 diffuse = gAlbedo.Load(int3(full, 0)).rgb * (1.0 - position.w);
        radianceOut[pixel] = float4(m * diffuse, 1.0);
        return;
    }

    radianceOut[pixel] = float4(mean, 1.0);
    // The denoiser's guide, as RT shadows pack theirs: the distance from the camera
    // (bfloat16, never 0 on a surface) and the octahedral normal (8 + 8 bits). 0 is sky.
    const uint dist = max(asuint(distance(p, pc.cameraPos.xyz)) >> 16u, 1u);
    const uint2 oct = uint2(saturate(octN) * 255.0 + 0.5);
    guideOut[pixel] = dist | (oct.x << 16u) | (oct.y << 24u);
}
