// Auto exposure (frontier F2 T2, ruling F-R5): a window's camera meters the frame it shows.
//
// histogram: every pixel's log2 luminance into kExposureBins bins over [minLog2, minLog2 + range],
//   each bin holding its pixel count and the sum of their log2 luminance in 1/16 EV (integers:
//   order-free, so the result is exact and repeatable; a frame twice as bright sums exactly 16 more
//   a pixel, short of the range's ends).
// adapt: one thread. The mean log2 luminance of the pixels between the low and high percentiles
//   (a bin straddling one counts in proportion), metered so that it displays at middle grey 0.18
//   (the zone system's key: EV100 = mean + log2(1 / (0.18 x 1.2))), less the compensation; the
//   camera's EV100 eases toward it (faster toward a brighter frame), and the bins are cleared for
//   the next frame. The first frame after a reset takes the target at once.

static const uint kExposureBins = 128u;

[[vk::binding(0, 0)]] Sampler2D hdr;                         // the frame the display pass shows
[[vk::binding(1, 0)]] RWStructuredBuffer<int> bins;          // kExposureBins counts, then kExposureBins sums (1/16 EV)
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> state;      // x EV100, y 1 once metered

struct ExposureParams
{
    uint2 size;
    float minLog2;       // the histogram's range in log2 luminance
    float rangeLog2;
    float lowFraction;   // the percentiles kept
    float highFraction;
    float compensation;  // EV: positive brightens
    float dt;            // seconds since the last frame
    float speedUp;       // 1/s toward a brighter frame (a lower EV100)
    float speedDown;     // 1/s toward a darker one
    uint reset;          // 1: take the target at once
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<ExposureParams> pc;

groupshared int gCount[kExposureBins];
groupshared int gSum[kExposureBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void histogram(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < kExposureBins) { gCount[index] = 0; gSum[index] = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < pc.size))
    {
        const float3 c = hdr.Load(int3(id.xy, 0)).rgb;
        const float lum = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
        const float l = clamp(log2(max(lum, 1e-10)), pc.minLog2, pc.minLog2 + pc.rangeLog2);
        const uint bin = min(uint((l - pc.minLog2) / pc.rangeLog2 * float(kExposureBins)), kExposureBins - 1u);
        InterlockedAdd(gCount[bin], 1);
        InterlockedAdd(gSum[bin], int(round(l * 16.0)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < kExposureBins && gCount[index] != 0)
    {
        InterlockedAdd(bins[index], gCount[index]);
        InterlockedAdd(bins[kExposureBins + index], gSum[index]);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void adapt()
{
    int total = 0;
    for (uint b = 0u; b < kExposureBins; ++b) total += bins[b];
    const float lo = float(total) * pc.lowFraction, hi = float(total) * pc.highFraction;
    float seen = 0.0, kept = 0.0, sum = 0.0;
    for (uint b = 0u; b < kExposureBins; ++b)
    {
        const float n = float(bins[b]);
        if (n > 0.0)
        {
            const float overlap = max(min(seen + n, hi) - max(seen, lo), 0.0);
            kept += overlap;
            sum += float(bins[kExposureBins + b]) / 16.0 * (overlap / n);
        }
        seen += n;
    }
    for (uint b = 0u; b < 2u * kExposureBins; ++b) bins[b] = 0;

    float4 s = state[0];
    if (kept <= 0.0) return;   // an empty frame meters nothing
    const float target = sum / kept + log2(1.0 / (0.18 * 1.2)) - pc.compensation;
    if (pc.reset != 0u || s.y == 0.0)
    {
        s = float4(target, 1.0, 0.0, 0.0);
    }
    else
    {
        const float speed = target < s.x ? pc.speedUp : pc.speedDown;
        s.x += (target - s.x) * (1.0 - exp(-pc.dt * speed));
    }
    state[0] = s;
}
