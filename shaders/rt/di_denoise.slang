// ReSTIR DI's denoiser (frontier F4, rt/restir_di.slang): the shaded frame's demodulated diffuse and
// specular light, four compute dispatches timed together as di.denoise.
//
// temporal  Each signal, a firefly clamped first (a pixel over twice the brightest of its 8 neighbours
//           taken down to that: the specular as far as the surface is rough), joins
//           its accumulation of last frame, reprojected by the G-buffer's velocity
//           (bilinear, each tap kept only where last frame's surface, restir_di's, is the same), blended by 1 / (frames + 1) down to 1 / kDIHistoryFrames. Alpha holds the
//           frames (none after a temporal reset or a disocclusion).
// atrous    Three times (steps 1, 2 and 4): a 3x3 a-trous kernel (1/4, 1/2, 1/4) over both
//           accumulations, as far as the 5x5 B3 kernel twice in about half its taps,
//           edge-stopped by the distance from the camera and the normal; the specular only as far
//           as the surface is rough (a mirror's reflection of a small light stays where it is).
//
// The guides are restir_di's (this frame's and last frame's): the distance from the camera and the
// packed normal, 8 bytes a tap.
// Lighting multiplies the result by the lobes' directional albedos. Every output depends only on its
// inputs: no atomics, no order.
import shared.layout;
import includes.lighting.gbuffer_material;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisyDiffuse;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisySpecular;
[[vk::binding(2, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> prevGuide;   // (distance; 0 none, packed normal)
[[vk::binding(3, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> guide;
[[vk::binding(4, 0)]] Texture2D<float2> gVelocity;   // UV motion: current minus previous
[[vk::binding(14, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> surfB;   // restir_di's: y the GGX alpha
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> histPrevDiffuse;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> histPrevSpecular;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inDiffuse;    // temporal: this frame's history out
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inSpecular;   // atrous: the input
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDiffuse;  // atrous: the output
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outSpecular;
[[vk::push_constant]] ConstantBuffer<DIPush> pc;

static const uint kFlagHistory = 2u;

struct Guide
{
    float3 normal;
    float dist;    // from the camera; 0 the sky or an emitter mesh
};

float3 unpackNormal(uint v)
{
    return decodeNormalOctahedron(float2(float(v & 0xFFFFu), float(v >> 16u)) / 65535.0);
}

Guide guideOf(float2 v)
{
    Guide g;
    g.dist = v.x;
    g.normal = unpackNormal(asuint(v.y));
    return g;
}

Guide guideAt(int2 q) { return guideOf(guide[q]); }

// The specular's spread at a pixel: none below roughness 0.05, all of the kernel from 0.3.
float specularSpread(int2 q) { return smoothstep(0.05, 0.3, sqrt(surfB[q].y)); }

// ---- temporal -------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void temporal(uint3 id : SV_DispatchThreadID)
{
    const int2 q = int2(id.xy);
    const int2 size = int2(pc.extent.xy);
    if (any(q >= size)) return;
    float4 d = noisyDiffuse[q], s = noisySpecular[q];
    const Guide g = guideAt(q);
    {
        float3 maxD = float3(0.0), maxS = float3(0.0);
        for (int k = 0; k < 9; ++k)
        {
            const int2 t = clamp(q + int2(k % 3 - 1, k / 3 - 1), int2(0), size - 1);
            if (k == 4) continue;
            maxD = max(maxD, noisyDiffuse[t].rgb);
            maxS = max(maxS, noisySpecular[t].rgb);
        }
        d.rgb = min(d.rgb, 2.0 * maxD);
        s.rgb = lerp(s.rgb, min(s.rgb, 2.0 * maxS), specularSpread(q));
    }
    float4 hd = float4(0.0), hs = float4(0.0);
    float frames = 0.0;
    if (g.dist > 0.0 && (pc.params.z & kFlagHistory) != 0u)
    {
        const float2 uv = (float2(q) + 0.5) / float2(size);
        const float2 p = (uv - gVelocity.Load(int3(q, 0))) * float2(size) - 0.5;
        const int2 base = int2(floor(p));
        const float2 f = p - float2(base);
        float wsum = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = base + o;
            if (any(t < 0) || any(t >= size)) continue;
            const Guide pg = guideOf(prevGuide[t]);
            if (!(pg.dist > 0.0) || abs(pg.dist - g.dist) > 0.1 * g.dist || dot(pg.normal, g.normal) < 0.9) continue;
            const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
            if (!(w > 0.0)) continue;
            hd += w * histPrevDiffuse[t];
            hs += w * histPrevSpecular[t];
            wsum += w;
        }
        if (wsum > 1e-3)
        {
            hd /= wsum;
            hs /= wsum;
            frames = hd.a;
        }
    }
    const float alpha = max(1.0 / (frames + 1.0), 1.0 / float(kDIHistoryFrames));
    const float n = min(frames + 1.0, float(kDIHistoryFrames));
    inDiffuse[q] = float4(frames > 0.0 ? lerp(hd.rgb, d.rgb, alpha) : d.rgb, n);
    inSpecular[q] = float4(frames > 0.0 ? lerp(hs.rgb, s.rgb, alpha) : s.rgb, n);
}

// ---- atrous ---------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void atrous(uint3 id : SV_DispatchThreadID)
{
    const int2 q = int2(id.xy);
    const int2 size = int2(pc.extent.xy);
    if (any(q >= size)) return;
    const Guide g = guideAt(q);
    const float4 cd = inDiffuse[q], cs = inSpecular[q];
    if (!(g.dist > 0.0))
    {
        outDiffuse[q] = cd;
        outSpecular[q] = cs;
        return;
    }
    const int step = int(max(pc.params.w, 1u));
    const float spread = specularSpread(q);
    static const float kernel[2] = {1.0 / 2.0, 1.0 / 4.0};
    float3 sd = float3(0.0), ss = float3(0.0);
    float wsum = 0.0, wsumS = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 t = q + int2(x, y) * step;
            if (any(t < 0) || any(t >= size)) continue;
            const Guide h = guideAt(t);
            if (!(h.dist > 0.0)) continue;
            const float wd = exp(-abs(h.dist - g.dist) / (0.02 * g.dist + 1e-4));
            const float wn = pow(saturate(dot(h.normal, g.normal)), 32.0);
            const float w = kernel[abs(x)] * kernel[abs(y)] * wd * wn;
            const float wS = x == 0 && y == 0 ? w : w * spread;
            sd += w * inDiffuse[t].rgb;
            ss += wS * inSpecular[t].rgb;
            wsum += w;
            wsumS += wS;
        }
    }
    outDiffuse[q] = float4(wsum > 0.0 ? sd / wsum : cd.rgb, cd.a);
    outSpecular[q] = float4(wsumS > 0.0 ? ss / wsumS : cs.rgb, cs.a);
}
