// RT GI's denoiser (sub-project 2b spec, "RT GI", Denoise): four compute dispatches after the
// realtime trace (gi_trace.slang), timed together as rt.gi.denoise.
//
// temporal  At half resolution, the trace's noisy radiance joins two accumulations of last
//           frame, reprojected by the G-buffer's velocity (bilinear, each tap kept only where
//           last frame's guide is the same surface: depth and normal): a short one (blend
//           weight down to 1/kGIFastHistoryFrames) and a long one (down to
//           1/kGIHistoryFrames). The long history is clamped to the range of the short one
//           over the 3x3 neighbourhood first, so a light that moves or changes drags at most
//           a few frames behind instead of kGIHistoryFrames. The short accumulation's alpha
//           holds the frames of history (none after a temporal reset or a disocclusion; the
//           two start and reproject together, so one count serves both); the long one's alpha
//           the environment's visibility, accumulated as its radiance is.
// atrous    Twice (steps 1 and 2), at half resolution: a 5x5 B3-spline a-trous kernel over
//           the long accumulation (radiance and visibility), edge-stopped by depth and normal.
//           The reflection (frontier F5b, params.w 1) runs the same chain with its roughness in
//           alpha: its kernel spreads only as far as the surface is rough (none below 0.05, all
//           from 0.4), so a mirror keeps its reflection sharp.
// upsample  To full resolution, joint-bilateral: the 4 nearest half-resolution samples
//           weighted bilinearly and by how well their depth and normal match the pixel's.
//           Lighting multiplies the radiance by the diffuse albedo and occludes the specular
//           IBL by the visibility.
//
// Every output depends only on its inputs: no atomics, no order. Each entry point has its own
// binding numbers (temporal 0-7, atrous 8-10, upsample 11-16) and its own descriptor set layout.
import shared.layout;

float3 decodeOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// xyz the normal, w the distance from the camera, from a guide word (gi_trace.slang); 0 is sky.
float4 decodeGuide(uint g)
{
    const float dist = asfloat((g & 0xFFFFu) << 16u);
    return float4(decodeOctahedron(float2(float(g >> 16u & 0xFFu), float(g >> 24u)) / 255.0), dist);
}

[[vk::push_constant]] ConstantBuffer<GIDenoisePush> pc;

// ---------------------------------------------------------------------------------------
// temporal

[[vk::binding(0, 0)]] Texture2D<float4> tNoisy;       // this frame's trace
[[vk::binding(1, 0)]] Texture2D<uint> tGuide;         // this frame's guide
[[vk::binding(2, 0)]] Texture2D<uint> tGuidePrev;     // last frame's guide
[[vk::binding(3, 0)]] Texture2D<float2> tVelocity;    // full resolution, UV motion: current minus previous
[[vk::binding(4, 0)]] Texture2D<float4> tFastPrev;    // last frame's short accumulation (a: frames)
[[vk::binding(5, 0)]] Texture2D<float4> tSlowPrev;    // last frame's long accumulation (a: frames)
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> tFastOut;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> tSlowOut;

static const uint kApron = 1u;
static const uint kSide = kGITile + 2u * kApron;
groupshared float4 gFast[kSide * kSide];   // this frame's short accumulation; a 0 for sky
groupshared float4 gSlow[kSide * kSide];   // last frame's long accumulation, reprojected; a 0 without one

// Last frame's two accumulations at half-resolution pixel q, whose guide is gc: bilinear
// over the taps that saw the same surface, renormalized. Both 0 when none did.
void reproject(int2 q, float4 gc, out float4 fast, out float4 slow)
{
    fast = float4(0.0);
    slow = float4(0.0);
    const int2 halfSize = int2(pc.extent.xy);
    const int2 full = int2(pc.extent.zw);
    const uint shift = pc.params.z;   // the trace is the frame >> shift (1 half, 0 full)
    const int2 p = min(q << int(shift), full - 1);
    const float2 uv = (float2(p) + 0.5) / float2(full);
    const float2 prevUv = uv - tVelocity.Load(int3(p, 0));
    // In last frame's trace samples. The half-resolution expression is kept exactly as it was
    // (a multiply by a selected factor compiles differently and moved a few pixels by 1 LSB).
    float2 hp;
    if (shift != 0u)
        hp = (prevUv * float2(full) - 0.5) * 0.5;
    else
        hp = prevUv * float2(full) - 0.5;
    const int2 base = int2(floor(hp));
    const float2 f = hp - float2(base);
    float wsum = 0.0;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const int2 o = int2(i & 1u, i >> 1u);
        const int2 tap = base + o;
        if (any(tap < int2(0)) || any(tap >= halfSize)) continue;
        const uint g = tGuidePrev.Load(int3(tap, 0));
        if (g == 0u) continue;
        const float4 gp = decodeGuide(g);
        if (abs(gp.w - gc.w) > 0.1 * gc.w || dot(gp.xyz, gc.xyz) < 0.9) continue;
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        if (w <= 0.0) continue;
        fast += w * tFastPrev.Load(int3(tap, 0));
        slow += w * tSlowPrev.Load(int3(tap, 0));
        wsum += w;
    }
    if (wsum < 0.05)   // a sliver of a tap is not a history
    {
        fast = float4(0.0);
        slow = float4(0.0);
        return;
    }
    fast /= wsum;
    slow /= wsum;
    fast.a = floor(fast.a + 0.5);   // a frame count; slow.a (the visibility) stays a mean
}

[shader("compute")]
[numthreads(kGITile, kGITile, 1)]
void temporal(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    const int2 size = int2(pc.extent.xy);
    const int2 origin = int2(group.xy * kGITile) - int2(kApron);
    const uint flat = local.y * kGITile + local.x;
    const bool history = pc.params.x > 0u;
    for (uint i = flat; i < kSide * kSide; i += kGITile * kGITile)
    {
        const int2 q = clamp(origin + int2(i % kSide, i / kSide), int2(0), size - 1);
        const uint g = tGuide.Load(int3(q, 0));
        float4 fast = float4(0.0), slow = float4(0.0);
        if (g != 0u)
        {
            const float3 cur = tNoisy.Load(int3(q, 0)).rgb;
            if (history) reproject(q, decodeGuide(g), fast, slow);
            // fast.a is the frames of history (both accumulations have the same: they start and
            // are reprojected together); the short one blends over at most kGIFastHistoryFrames.
            const float n = min(fast.a + 1.0, float(kGIFastHistoryFrames));
            fast = float4(lerp(fast.rgb, cur, 1.0 / n), min(fast.a + 1.0, float(kGIHistoryFrames)));
        }
        gFast[i] = fast;
        gSlow[i] = slow;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = group.xy * kGITile + local.xy;
    if (any(pixel >= uint2(size))) return;
    const uint ci = (local.y + kApron) * kSide + local.x + kApron;
    const float4 fast = gFast[ci];
    if (fast.a == 0.0)   // sky
    {
        tFastOut[pixel] = float4(0.0);
        tSlowOut[pixel] = float4(0.0);
        return;
    }
    // The short accumulation's range over the 3x3 (surface pixels only) bounds the long history.
    float3 lo = fast.rgb, hi = fast.rgb;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const float4 v = gFast[uint(int(ci) + dy * int(kSide) + dx)];
            if (v.a == 0.0) continue;
            lo = min(lo, v.rgb);
            hi = max(hi, v.rgb);
        }
    }
    const float4 cur = tNoisy.Load(int3(pixel, 0));
    const float4 slowPrev = gSlow[ci];
    const float n = fast.a;   // min(frames of history + 1, kGIHistoryFrames)
    const float3 slow = lerp(clamp(slowPrev.rgb, lo, hi), cur.rgb, 1.0 / n);
    const float visibility = lerp(slowPrev.a, cur.a, 1.0 / n);
    tFastOut[pixel] = fast;
    tSlowOut[pixel] = float4(slow, visibility);
}

// ---------------------------------------------------------------------------------------
// atrous

[[vk::binding(8, 0)]] Texture2D<float4> aInput;
[[vk::binding(9, 0)]] Texture2D<uint> aGuide;
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> aOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void atrous(uint3 id : SV_DispatchThreadID)
{
    const int2 size = int2(pc.extent.xy);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= size)) return;
    const uint gcWord = aGuide.Load(int3(pixel, 0));
    if (gcWord == 0u)
    {
        aOutput[pixel] = float4(0.0);
        return;
    }
    const float4 gc = decodeGuide(gcWord);
    const int step = int(pc.params.y);
    // Depth tolerance grows with the offset, so a plane seen at a grazing angle (to about
    // 80 degrees at 720p) keeps its neighbours.
    const float invTol = 1.0 / (0.03 * gc.w * float(step));
    const float kH[3] = {0.375, 0.25, 0.0625};   // B3 spline: centre, 1, 2
    const float spread = pc.params.w == 1u ? smoothstep(0.05, 0.4, aInput.Load(int3(pixel, 0)).a) : 1.0;
    float4 sum = float4(0.0);
    float wsum = 0.0;
    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int2 q = pixel + int2(dx, dy) * step;
            if (any(q < int2(0)) || any(q >= size)) continue;
            const uint g = aGuide.Load(int3(q, 0));
            if (g == 0u) continue;
            float w = kH[abs(dx)] * kH[abs(dy)];
            if (dx != 0 || dy != 0)
            {
                const float4 gq = decodeGuide(g);
                w *= exp(-abs(gq.w - gc.w) * invTol / max(abs(dx), abs(dy)));
                if ((g >> 16u) != (gcWord >> 16u))
                {
                    const float nd = saturate(dot(gq.xyz, gc.xyz));
                    const float nd2 = nd * nd;
                    const float nd4 = nd2 * nd2;
                    w *= nd4 * nd4 * nd4 * nd4;   // cos^16
                }
                w *= spread;
            }
            sum += w * aInput.Load(int3(q, 0));
            wsum += w;
        }
    }
    aOutput[pixel] = sum / wsum;   // the centre is in: wsum >= 0.14
}

// ---------------------------------------------------------------------------------------
// upsample

[[vk::binding(11, 0)]] Texture2D<float4> uInput;       // half resolution, filtered
[[vk::binding(12, 0)]] Texture2D<uint> uGuide;         // half resolution
[[vk::binding(13, 0)]] Texture2D<float4> uPosition;    // G-buffer: xyz position, w metallic
[[vk::binding(14, 0)]] Texture2D<float4> uNormal;      // G-buffer: xy octahedral normal
[[vk::binding(15, 0)]] Texture2D<float4> uAlbedo;      // G-buffer: rgb albedo
[[vk::binding(16, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> uOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void upsample(uint3 id : SV_DispatchThreadID)
{
    const int2 full = int2(pc.extent.zw);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= full)) return;
    const float4 position = uPosition.Load(int3(pixel, 0));
    if (all(position == float4(0.0)))
    {
        uOutput[pixel] = float4(0.0);
        return;
    }
    const float dc = distance(position.xyz, pc.cameraPos.xyz);
    const float3 nc = decodeOctahedron(uNormal.Load(int3(pixel, 0)).xy);
    const int2 halfSize = int2(pc.extent.xy);
    // Half-resolution sample q sits on full-resolution pixel 2q; at full resolution (shift 0)
    // the sample is the pixel and the taps past it weigh 0.
    const uint shift = pc.params.z;
    const int2 base = pixel >> int(shift);
    const float2 f = shift != 0u ? float2(pixel & 1) * 0.5 : float2(0.0);
    const float invTol = 1.0 / (0.05 * dc);
    float4 sum = float4(0.0), best = float4(0.0);
    float wsum = 0.0, bestW = -1.0;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const int2 o = int2(i & 1u, i >> 1u);
        const int2 tap = min(base + o, halfSize - 1);
        const uint g = uGuide.Load(int3(tap, 0));
        if (g == 0u) continue;
        const float4 gq = decodeGuide(g);
        const float nd = saturate(dot(gq.xyz, nc));
        const float nd2 = nd * nd;
        const float match = exp(-abs(gq.w - dc) * invTol) * (nd2 * nd2 * nd2 * nd2);   // cos^8
        const float4 v = uInput.Load(int3(tap, 0));
        if (match > bestW)
        {
            bestW = match;
            best = v;
        }
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * match;
        sum += w * v;
        wsum += w;
    }
    // No tap is this surface (a sliver the half-resolution trace missed): the closest match.
    uOutput[pixel] = wsum > 1e-3 ? sum / wsum : best;
}
