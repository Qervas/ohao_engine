// The radiance cache's resolve (rt/radiance_cache.slang): one thread a slot, after the update pass.
// A slot that received samples this frame blends their mean into its radiance, weighted by their
// count against the count it holds (at most maxSamples, so old light fades within that many
// samples); one that received none ages, and a slot unseen for staleFrames frames is freed. The
// frame's sums are cleared for the next update.
import rt.radiance_cache;

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> keys;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> accum;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> resolved;

struct ResolveConstants
{
    uint capacity;
    uint maxSamples;
    uint staleFrames;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<ResolveConstants> rc;

[shader("compute")]
[numthreads(256, 1, 1)]
void resolve(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x;
    if (slot >= rc.capacity || keys[slot] == 0u) return;
    const uint4 a = accum[slot];
    uint4 r = resolved[slot];
    uint count = r.w & 0xFFFFu;
    uint age = r.w >> 16u;
    if (a.w > 0u)
    {
        const float3 mean = float3(a.xyz) / (kCacheScale * float(a.w));
        const float3 old = asfloat(r.xyz);
        const float total = float(count + a.w);
        const float3 radiance = count > 0u ? (old * float(count) + mean * float(a.w)) / total : mean;
        count = min(count + a.w, rc.maxSamples);
        age = 0u;
        r.xyz = asuint(radiance);
    }
    else
    {
        age = min(age + 1u, 0xFFFFu);
    }
    if (age > rc.staleFrames)
    {
        keys[slot] = 0u;
        resolved[slot] = uint4(0u);
    }
    else
    {
        r.w = (age << 16u) | count;
        resolved[slot] = r;
    }
    accum[slot] = uint4(0u);
}
