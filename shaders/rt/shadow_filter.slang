// RT shadows' filter (sub-project 2b spec, "RT shadows", Filter): one compute pass after the
// realtime trace (shadow_trace.slang). It turns the trace's one ray a light a pixel into the
// visibility the lighting pass reads: eight 8-bit channels, one a light, in an RG32UI image
// (x: lights 0-3, y: 4-7), rounded with a hash dither so the temporal mean stays unbiased:
//
// spatial   a 5x5 kernel over the pixels that traced the same light, weighted by distance
//           from the centre and edge-stopped by depth (the distance from the camera) and
//           normal, from a tile the workgroup decodes once into shared memory;
// temporal  blended with last frame's output, reprojected by the G-buffer's velocity and
//           rejected where the history pixel is another surface (depth, normal) or did not
//           trace the light. The blend weight is 1 / (history frames + 1), down to
//           1 / kShadowHistoryFrames: a still camera after a reset averages its frames evenly.
//
// A light the pixel did not trace (out of range, behind it, or past the per-pixel cap) is
// lit: 1. Sky pixels are 1. Every output depends only on its inputs: no atomics, no order.
// Per-light values live in float4s (lights 0-3, 4-7), never in arrays a runtime index reads.
// The pass is bound by memory, not arithmetic: 8 bytes of history and 8 of output a pixel.
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<uint2> metaCur;      // this frame's trace
[[vk::binding(1, 0)]] Texture2D<uint2> metaPrev;     // last frame's trace (its guide and traced bits)
[[vk::binding(2, 0)]] Texture2D<float2> velocity;    // UV-space motion, current minus previous
[[vk::binding(3, 0)]] Texture2D<uint2> history;     // last frame's output
[[vk::binding(4, 0)]] [[vk::image_format("rg32ui")]] RWTexture2D<uint2> outVis;
[[vk::push_constant]] ConstantBuffer<ShadowFilterPush> pc;

static const uint kApron = kShadowFilterRadius;
static const uint kSide = kShadowFilterTile + 2u * kApron;
groupshared uint gGuide[kSide * kSide];   // the trace's guide word (distance, octahedral normal)
groupshared uint gBits[kSide * kSide];    // traced (bits 0-7) and visible (bits 8-15) per light; 0 for sky

float3 decodeOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

bool isSurface(uint2 m) { return (m.y >> 16u & 1u) != 0u; }

// xyz the normal, w the distance from the camera, from a surface's guide word.
float4 decodeGuide(uint g)
{
    const float dist = asfloat((g & 0xFFFFu) << 16u);   // bfloat16 (shadow_trace.slang)
    return float4(decodeOctahedron(float2(float(g >> 16u & 0xFFu), float(g >> 24u)) / 255.0), dist);
}

// Eight 8-bit channels in a uint2 (shadow_trace.slang packs the reference the same way).
float4 unpack4(uint p) { return float4((uint4(p) >> uint4(0u, 8u, 16u, 24u)) & 255u) * (1.0 / 255.0); }
// Rounded down after adding `dither` in [0, 1): unbiased over frames. 0 and 1 stay exact.
uint pack4(float4 v, float dither)
{
    const uint4 q = uint4(min(saturate(v) * 255.0 + dither, 255.0));
    return q.x | (q.y << 8u) | (q.z << 16u) | (q.w << 24u);
}

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float4 bits4(uint b, uint shift) { return float4(uint4(b >> shift, b >> (shift + 1u), b >> (shift + 2u), b >> (shift + 3u)) & 1u); }

[shader("compute")]
[numthreads(kShadowFilterTile, kShadowFilterTile, 1)]
void filter(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    const int2 size = int2(pc.extent.xy);
    const int2 origin = int2(group.xy * kShadowFilterTile) - int2(kApron);
    const uint flat = local.y * kShadowFilterTile + local.x;
    for (uint i = flat; i < kSide * kSide; i += kShadowFilterTile * kShadowFilterTile)
    {
        const int2 q = clamp(origin + int2(i % kSide, i / kSide), int2(0), size - 1);
        const uint2 m = metaCur.Load(int3(q, 0));
        gGuide[i] = m.x;
        gBits[i] = isSurface(m) ? (m.y & 0xFFFFu) : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = group.xy * kShadowFilterTile + local.xy;
    if (any(pixel >= uint2(size))) return;

    const uint ci = (local.y + kApron) * kSide + local.x + kApron;
    const uint tracedC = gBits[ci] & 0xFFu;   // 0 for sky
    float4 vLo = float4(1.0), vHi = float4(1.0);
    if (tracedC != 0u)
    {
        const float4 gc = decodeGuide(gGuide[ci]);
        // Where every pixel of the 5x5 that traced a light saw the same thing, the weighted
        // mean is that value whatever the weights: only lights seen both lit and occluded
        // (penumbrae, noise) pay for the edge-stopped kernel.
        const int r = int(kApron);
        uint anyLit = 0u, anyHidden = 0u;
        [unroll]
        for (int dy = -r; dy <= r; ++dy)
        {
            [unroll]
            for (int dx = -r; dx <= r; ++dx)
            {
                const uint b = gBits[uint(int(ci) + dy * int(kSide) + dx)];
                anyLit |= b & (b >> 8u);
                anyHidden |= b & ~(b >> 8u);
            }
        }
        anyLit &= tracedC;
        anyHidden &= tracedC;
        const uint mixed = anyLit & anyHidden;
        vLo = select(bits4(anyHidden & ~anyLit, 0u) > 0.5, float4(0.0), float4(1.0));
        vHi = select(bits4(anyHidden & ~anyLit, 4u) > 0.5, float4(0.0), float4(1.0));

        if (mixed != 0u)
        {
            // A pixel traces at most kShadowLightsPerPixel lights, so at most that many are
            // mixed: their indices go in one uint4 (31 for an empty slot, whose bits are 0), and
            // one float4 accumulates them whatever channels they are.
            uint4 idx = uint4(31u);
            uint m = mixed;
            [unroll]
            for (uint k = 0u; k < kShadowLightsPerPixel; ++k)
            {
                if (m == 0u) break;
                idx[k] = firstbitlow(m);
                m &= m - 1u;
            }
            const uint4 visIdx = min(idx + 8u, uint4(31u));
            float4 sum = float4(0.0), wsum = float4(0.0);
            const float invTol = 1.0 / (0.05 * gc.w + 1e-3);
            [unroll]
            for (int dy = -r; dy <= r; ++dy)
            {
                [unroll]
                for (int dx = -r; dx <= r; ++dx)
                {
                    const uint ni = uint(int(ci) + dy * int(kSide) + dx);
                    const uint b = gBits[ni];
                    if ((b & mixed) == 0u) continue;   // sky and pixels that traced none of the mixed lights
                    const int ring = max(abs(dx), abs(dy));
                    float w = ring == 0 ? 1.0 : ring == 1 ? 0.6 : 0.3;
                    if (ring != 0)
                    {
                        // Depth from the bfloat16 distance; the normal decoded only where its
                        // 16 bits differ from the centre's (on a flat surface they rarely do).
                        const uint go = gGuide[ni];
                        w *= saturate(1.0 - abs(asfloat((go & 0xFFFFu) << 16u) - gc.w) * invTol);
                        if ((go >> 16u) != (gGuide[ci] >> 16u))
                        {
                            const float nd = saturate(dot(gc.xyz, decodeGuide(go).xyz));
                            const float nd2 = nd * nd;
                            w *= nd2 * nd2;   // cos^4
                        }
                    }
                    const float4 traced = float4((uint4(b) >> idx) & 1u);
                    wsum += w * traced;
                    sum += w * traced * float4((uint4(b) >> visIdx) & 1u);
                }
            }
            // The centre is in its own sum: a mixed light's weight is at least 1.
            const float4 mean = sum / max(wsum, 1e-6);
            [unroll]
            for (uint k = 0u; k < kShadowLightsPerPixel; ++k)
            {
                if (idx[k] == 31u) break;
                if (idx[k] < 4u) vLo = select(uint4(0u, 1u, 2u, 3u) == idx[k], float4(mean[k]), vLo);
                else vHi = select(uint4(4u, 5u, 6u, 7u) == idx[k], float4(mean[k]), vHi);
            }
        }

        // Temporal: last frame's output where the same surface traced the same light.
        if (pc.extent.w > 0u)
        {
            const float2 uv = (float2(pixel) + 0.5) / float2(size);
            const float2 prevUv = uv - velocity.Load(int3(pixel, 0));
            const int2 prev = int2(floor(prevUv * float2(size)));
            if (all(prev >= int2(0)) && all(prev < size))
            {
                const uint2 pm = metaPrev.Load(int3(prev, 0));
                const float4 gp = decodeGuide(pm.x);
                if (isSurface(pm) && abs(gp.w - gc.w) < 0.1 * gc.w && dot(gp.xyz, gc.xyz) > 0.9)
                {
                    const float alpha = 1.0 / float(min(pc.extent.w + 1u, kShadowHistoryFrames));
                    const uint both = tracedC & pm.y;
                    const uint2 h = history.Load(int3(prev, 0));
                    vLo = select(bits4(both, 0u) > 0.5, lerp(unpack4(h.x), vLo, alpha), vLo);
                    vHi = select(bits4(both, 4u) > 0.5, lerp(unpack4(h.y), vHi, alpha), vHi);
                }
            }
        }
    }
    const float dither = float(pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y)))) >> 8u) * (1.0 / 16777216.0);
    outVis[pixel] = uint2(pack4(vLo, dither), pack4(vHi, dither));
}
