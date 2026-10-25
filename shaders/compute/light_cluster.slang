// lights.cluster (sub-project 2b spec, "Clustered lighting"): every froxel's light list.
// Froxels are kClusterTilesX x kClusterTilesY screen tiles by kClusterSlices exponential
// depth slices; one workgroup a froxel tests every light, kClusterGroupSize at a time, and
// appends the ones that can reach it in light order (a compaction in shared memory, no
// atomics across workgroups), so the lists are the same every run. core/deferred_lighting.frag
// sums a pixel's froxel's lights in that order.
//
// Exactness: a point or spot light contributes exactly zero past its range (the attenuation's
// window), so a pixel summing only the lights whose range sphere reaches its froxel sums the
// same terms, in the same order, as one summing every light. The froxel's box is made
// conservative for that: the tile 1.5 pixels wider on each side (the sample jitter and
// rounding), the slice 0.1% deeper each way, the range 0.01% longer.
//
// Output (`clusters`): kClusterCount counts, then kClusterMaxLights light indices a froxel.
// A froxel reached by more keeps the first kClusterMaxLights; `stats` sums the rest (x: the
// lights dropped, y: the froxels that dropped any, z: the largest count, before the cut).
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<LightGpu> lights;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> clusters;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> stats;
[[vk::push_constant]] ConstantBuffer<ClusterPush> pc;

groupshared uint gMask[kClusterGroupSize / 32u];

// A view-space point of the froxel's frustum: ndc on the screen, d metres in front.
float3 cornerAt(float2 ndc, float d)
{
    return float3(d * (ndc.x + pc.proj.z) / pc.proj.x, d * (ndc.y + pc.proj.w) / pc.proj.y, -d);
}

[shader("compute")]
[numthreads(kClusterGroupSize, 1, 1)]
void build(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint cluster = gid.x;
    const uint tx = cluster % kClusterTilesX;
    const uint ty = (cluster / kClusterTilesX) % kClusterTilesY;
    const uint slice = cluster / (kClusterTilesX * kClusterTilesY);
    const uint t = tid.x;

    // The froxel's view-space box: the tile (1.5 pixels wider each side) between the slice's
    // depths (0.1% wider each way; the first slice from the eye, the last to 1.001 x far).
    const float2 size = pc.screen.xy;
    const float2 tile = size / float2(kClusterTilesX, kClusterTilesY);
    const float2 p0 = float2(tx, ty) * tile - 1.5, p1 = float2(tx + 1u, ty + 1u) * tile + 1.5;
    const float2 n0 = p0 / size * 2.0 - 1.0, n1 = p1 / size * 2.0 - 1.0;
    const float dNear = slice == 0u ? 0.0 : exp((float(slice) - pc.screen.w) / pc.screen.z) * 0.999;
    const float dFar = exp((float(slice + 1u) - pc.screen.w) / pc.screen.z) * 1.001;
    float3 lo = float3(1e30), hi = float3(-1e30);
    for (uint k = 0u; k < 8u; ++k)
    {
        const float3 c = cornerAt(float2((k & 1u) != 0u ? n1.x : n0.x, (k & 2u) != 0u ? n1.y : n0.y),
                                  (k & 4u) != 0u ? dFar : dNear);
        lo = min(lo, c);
        hi = max(hi, c);
    }

    const uint count = pc.config.x;
    const uint listBase = kClusterCount + cluster * kClusterMaxLights;
    uint written = 0u;   // the same in every thread
    for (uint base = 0u; base < count; base += kClusterGroupSize)
    {
        const uint i = base + t;
        bool hit = false;
        if (i < count)
        {
            const LightGpu l = lights[i];
            if (l.position.w < 0.5)
            {
                hit = true;   // directional: everywhere
            }
            else
            {
                const float3 c = mul(pc.view, float4(l.position.xyz, 1.0)).xyz;
                const float r = l.direction.w * 1.0001 + 1e-4;
                const float3 q = clamp(c, lo, hi) - c;
                hit = dot(q, q) <= r * r;
            }
        }
        // In light order: each hit's place is the hits of lower threads before it.
        if (t < kClusterGroupSize / 32u) gMask[t] = 0u;
        GroupMemoryBarrierWithGroupSync();
        if (hit) InterlockedOr(gMask[t >> 5u], 1u << (t & 31u));
        GroupMemoryBarrierWithGroupSync();
        uint before = 0u, total = 0u;
        for (uint w = 0u; w < kClusterGroupSize / 32u; ++w)
        {
            const uint m = gMask[w];
            total += countbits(m);
            if (w < (t >> 5u)) before += countbits(m);
            else if (w == (t >> 5u)) before += countbits(m & ((1u << (t & 31u)) - 1u));
        }
        if (hit && written + before < kClusterMaxLights) clusters[listBase + written + before] = i;
        written += total;
        GroupMemoryBarrierWithGroupSync();   // every thread read the masks before the next round clears them
    }

    if (t == 0u)
    {
        clusters[cluster] = min(written, kClusterMaxLights);
        if (written > kClusterMaxLights)
        {
            InterlockedAdd(stats[0], written - kClusterMaxLights);
            InterlockedAdd(stats[1], 1u);
        }
        InterlockedMax(stats[2], written);
    }
}
