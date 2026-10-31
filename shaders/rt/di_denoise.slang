// ReSTIR DI's denoiser (frontier F4, rt/restir_di.slang): the shaded frame's demodulated diffuse and
// specular light, four compute dispatches timed together as di.denoise.
//
// temporal  Each signal joins its accumulation of last frame, reprojected by the G-buffer's velocity
//           (bilinear, each tap kept only where last frame's surface is the same), blended by
//           1 / (frames + 1) down to 1 / kDIHistoryFrames (no frames after a temporal reset or a
//           disocclusion). Each signal's mean square luminance accumulates beside it, with the
//           frames (SVGF, Schied et al. 2017; the mean is the accumulation's own luminance), and
//           alpha holds the deviation they give, so a-trous reads it with the colour.
// atrous    Three times (steps 1, 2 and 4): a 3x3 a-trous kernel (1/4, 1/2, 1/4) over both
//           accumulations, as far as the 5x5 B3 kernel twice in about half its taps,
//           edge-stopped by the distance from the camera, the normal and, once a pixel has
//           kDIVarianceFrames frames, the luminance: a tap further from the centre than
//           kDILuminancePhi x the samples' deviation (the larger of the two pixels') weighs less.
//           A noisy pixel keeps filtering; a quiet one stops smoothing across a sharp shadow's edge.
//           SVGF's stop, symmetric: with the centre's deviation alone a bright pixel took its dark
//           neighbours while they refused it (Sponza at night 3% darker); with the accumulation's
//           standard error (over sqrt(frames)) it stopped too hard and darkened soft penumbrae. The specular spreads only as far as the surface
//           is rough (a mirror's reflection of a small light stays where it is).
//
// The guides are restir_di's shade's (this frame's and last frame's): the distance from the camera,
// the normal and the specular's spread, 8 bytes a tap.
// Lighting multiplies the result by the lobes' directional albedos. Every output depends only on its
// inputs: no atomics, no order.
import shared.layout;
import includes.lighting.gbuffer_material;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisyDiffuse;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisySpecular;
[[vk::binding(2, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> prevGuide;   // (distance; 0 none, normal and spread)
[[vk::binding(3, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> guide;
[[vk::binding(4, 0)]] Texture2D<float2> gVelocity;   // UV motion: current minus previous
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> histPrevDiffuse;
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> histPrevSpecular;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inDiffuse;    // temporal: this frame's history out
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inSpecular;   // atrous: the input
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDiffuse;  // atrous: the output
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outSpecular;
// The accumulation's mean square luminance (x diffuse, y specular) and its frames (z): last frame's and
// this frame's. 32-bit: a square near a lamp is past half precision's range.
[[vk::binding(12, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> prevMoments;
[[vk::binding(13, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> moments;
[[vk::push_constant]] ConstantBuffer<DIPush> pc;

static const uint kFlagHistory = 2u;
static const float kDIVarianceFrames = 4.0;   // fewer frames: a variance too poor to stop on
static const float kDILuminancePhi = 4.0;     // SVGF's sigma_l

float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }


struct Guide
{
    float3 normal;
    float dist;      // from the camera; 0 the sky or an emitter mesh
    float spread;    // the specular's: none below roughness 0.05, all of the kernel from 0.3
};

// restir_di.slang's packGuide.
Guide guideOf(float2 v)
{
    const uint bits = asuint(v.y);
    Guide g;
    g.dist = v.x;
    g.normal = decodeNormalOctahedron(float2(float(bits & 0xFFFu), float((bits >> 12u) & 0xFFFu)) / 4095.0);
    g.spread = float(bits >> 24u) / 255.0;
    return g;
}

Guide guideAt(int2 q) { return guideOf(guide[q]); }

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
    float4 hd = float4(0.0), hs = float4(0.0), hm = float4(0.0);
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
            hm += w * prevMoments[t];
            wsum += w;
        }
        if (wsum > 1e-3)
        {
            hd /= wsum;
            hs /= wsum;
            hm /= wsum;
            frames = hm.z;
        }
    }
    const float alpha = max(1.0 / (frames + 1.0), 1.0 / float(kDIHistoryFrames));
    const float n = min(frames + 1.0, float(kDIHistoryFrames));
    const float3 md = frames > 0.0 ? lerp(hd.rgb, d.rgb, alpha) : d.rgb;
    const float3 ms = frames > 0.0 ? lerp(hs.rgb, s.rgb, alpha) : s.rgb;
    const float ld = lum(d.rgb), ls = lum(s.rgb);
    const float2 m2 = frames > 0.0 ? lerp(hm.xy, float2(ld * ld, ls * ls), alpha) : float2(ld * ld, ls * ls);
    const float2 mean = float2(lum(md), lum(ms));
    const float2 dev = sqrt(max(m2 - mean * mean, 0.0));
    inDiffuse[q] = float4(md, dev.x);
    inSpecular[q] = float4(ms, dev.y);
    moments[q] = float4(m2, n, 0.0);
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
    const float spread = g.spread;
    // The luminance stops (alpha: the samples' deviation), none on a short history.
    const bool stops = moments[q].z >= kDIVarianceFrames;
    const float lcd = lum(cd.rgb), lcs = lum(cs.rgb);
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
            const float wg = kernel[abs(x)] * kernel[abs(y)] * wd * wn;
            const float4 td = inDiffuse[t], ts = inSpecular[t];
            // Symmetric: the larger of the two pixels' deviations, so a pair weighs each other alike
            // (a bright pixel taking a dark neighbour while the dark one refuses it loses light).
            float w = wg, wS = x == 0 && y == 0 ? wg : wg * spread;
            if (stops && !(x == 0 && y == 0))
            {
                w *= exp(-abs(lum(td.rgb) - lcd) / (kDILuminancePhi * max(cd.a, td.a) + 1e-4));
                wS *= exp(-abs(lum(ts.rgb) - lcs) / (kDILuminancePhi * max(cs.a, ts.a) + 1e-4));
            }
            sd += w * td.rgb;
            ss += wS * ts.rgb;
            wsum += w;
            wsumS += wS;
        }
    }
    outDiffuse[q] = float4(wsum > 0.0 ? sd / wsum : cd.rgb, cd.a);
    outSpecular[q] = float4(wsumS > 0.0 ? ss / wsumS : cs.rgb, cs.a);
}
