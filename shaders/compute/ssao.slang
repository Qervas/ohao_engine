// SSAO (sub-project 2b spec, "SSAO"): horizon-based ambient occlusion at half resolution,
// two compute dispatches timed together as `ssao`. The lighting pass upsamples the result
// with depth awareness and multiplies only the ambient and environment terms by it.
//
// occlusion  Half-resolution pixel q shades full-resolution pixel 2q from the G-buffer's
//            world position and normal. config.x directions in the surface's tangent plane,
//            rotated by the 4x4 interleaved noise texel (q.x mod 4, q.y mod 4), each marched
//            config.y steps across the screen to the projection of a point `radius` metres
//            away; a direction's occlusion is the highest elevation (sine above the tangent
//            plane, seen from the point lifted `bias` along its normal) of the samples within
//            the radius, faded by distance. Writes (ao, distance from the camera).
// blur       A 4x4 window (offsets -1..2), depth-aware: every window holds each of the 16
//            noise texels exactly once, so the interleaving averages out. Writes the same
//            (ao, distance) pair lighting reads.
//
// The noise is 16 rotations from the render seed (SSAOPass::noise), packed 4 bits a texel in
// the push constants (128 bytes, the size Vulkan guarantees): deterministic, and reseeding
// uploads nothing.
import shared.layout;

[[vk::push_constant]] ConstantBuffer<SSAOPush> pc;

static const float kTwoPi = 6.2831853;

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

// The rotation of noise texel (q.x mod 4, q.y mod 4): its stratum, 4 bits in config.z/w.
float noiseTexel(uint2 q)
{
    const uint k = (q.x & 3u) | ((q.y & 3u) << 2u);
    const uint word = k < 8u ? pc.config.z : pc.config.w;
    return (float((word >> ((k & 7u) * 4u)) & 15u) + 0.5) / 16.0;
}

// ---------------------------------------------------------------------------------------
// occlusion

[[vk::binding(0, 0)]] Texture2D<float4> oPosition;   // G-buffer: xyz world position; (0,0,0,0) sky
[[vk::binding(1, 0)]] Texture2D<float4> oNormal;     // G-buffer: xy octahedral normal
[[vk::binding(2, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> oOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void occlusion(uint3 id : SV_DispatchThreadID)
{
    const uint2 q = id.xy;
    if (any(q >= pc.extent.xy)) return;
    const int2 full = int2(pc.extent.zw);
    const int2 p = min(int2(q * 2u), full - 1);
    const float4 position = oPosition.Load(int3(p, 0));
    if (all(position == float4(0.0)))
    {
        oOut[q] = float2(1.0, 0.0);
        return;
    }
    const float3 pos = position.xyz;
    const float3 n = decodeOctahedron(oNormal.Load(int3(p, 0)).xy);
    const float dist = distance(pos, pc.cameraPosRadius.xyz);
    const float radius = pc.cameraPosRadius.w;
    const float3 origin = pos + n * pc.params.x;
    float3 t, b;
    basis(n, t, b);

    const uint directions = max(pc.config.x, 1u);
    const uint steps = max(pc.config.y, 1u);
    const float rot = noiseTexel(q);
    const float jitter = frac(rot * 7.0 + 0.5);   // where along a step the first sample lands
    const float2 uv0 = (float2(p) + 0.5) / float2(full);
    float occluded = 0.0;
    for (uint d = 0u; d < directions; ++d)
    {
        const float angle = kTwoPi * (float(d) + rot) / float(directions);
        const float3 dir = t * cos(angle) + b * sin(angle);
        const float4 clip = mul(pc.viewProj, float4(pos + dir * radius, 1.0));
        if (clip.w <= 1e-4) continue;
        const float2 uvEnd = clip.xy / clip.w * 0.5 + 0.5;
        const float2 stepUv = (uvEnd - uv0) / float(steps);
        float best = 0.0;
        for (uint s = 0u; s < steps; ++s)
        {
            const int2 sp = int2(floor((uv0 + stepUv * (float(s) + jitter)) * float2(full)));
            if (any(sp < int2(0)) || any(sp >= full) || all(sp == p)) continue;
            const float4 sample = oPosition.Load(int3(sp, 0));
            if (all(sample == float4(0.0))) continue;
            const float3 v = sample.xyz - origin;
            const float l2 = dot(v, v);
            if (l2 < 1e-8 || l2 > radius * radius) continue;
            const float elevation = dot(v, n) * rsqrt(l2);
            best = max(best, elevation * (1.0 - l2 / (radius * radius)));
        }
        occluded += best;
    }
    float ao = saturate(1.0 - pc.params.y * occluded / float(directions));
    ao = lerp(1.0, ao, 1.0 - smoothstep(pc.params.z, pc.params.w, dist));
    oOut[q] = float2(ao, dist);
}

// ---------------------------------------------------------------------------------------
// blur

[[vk::binding(3, 0)]] Texture2D<float2> bIn;
[[vk::binding(4, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> bOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void blur(uint3 id : SV_DispatchThreadID)
{
    const int2 q = int2(id.xy);
    const int2 size = int2(pc.extent.xy);
    if (any(q >= size)) return;
    const float2 c = bIn.Load(int3(q, 0));
    if (c.y <= 0.0)   // sky
    {
        bOut[q] = c;
        return;
    }
    const float invTol = 1.0 / (0.05 * c.y);
    float sum = 0.0, wsum = 0.0;
    [unroll]
    for (int dy = -1; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 2; ++dx)
        {
            const int2 s = clamp(q + int2(dx, dy), int2(0), size - 1);
            const float2 v = bIn.Load(int3(s, 0));
            if (v.y <= 0.0) continue;
            const float w = saturate(1.0 - abs(v.y - c.y) * invTol);
            sum += w * v.x;
            wsum += w;
        }
    }
    bOut[q] = float2(sum / wsum, c.y);   // the centre is in: wsum >= 1
}
