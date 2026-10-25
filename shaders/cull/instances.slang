// GPU culling (sub-project 2b spec, "GPU-driven geometry"): RenderScene's draw table (one
// record an instance's submesh) to indirect draw commands, in three dispatches a phase.
//   classify     each draw's visibility for the phase (frustum, the Hi-Z pyramid, last frame's
//                visibility), and its offset in its workgroup's output by a workgroup scan
//   scanBlocks   one workgroup: the workgroups' totals to their output offsets, and the counts
//   scatter      each member draw's VkDrawIndexedIndirectCommand at its offset
// Output order is draw order, every run: a prefix sum, never an atomic append. A phase
// writes a pair of lists (one-sided draws, double-sided draws: face culling on and off);
// list L's commands start at uint L * drawCount * 5. firstInstance is the draw's index: the
// vertex shaders read their draw record, and through it the instance, with it.
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<DrawRecord> draws;
// The instance table read as float4s (kInstanceGpuStride / 16 a slot); the world matrix is
// the first four, its columns.
[[vk::binding(1, 0)]] StructuredBuffer<float4> instances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibility;    // a draw: 1 when the last late phase saw it
[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> local;        // a draw: its offset in its workgroup's lists, bit 31 membership
[[vk::binding(4, 0)]] RWStructuredBuffer<uint2> blockSums;    // a workgroup's member counts
[[vk::binding(5, 0)]] Texture2D<float> hiz;                   // max-depth pyramid; level L texel = 2^(L+1) pixels square
[[vk::binding(6, 0)]] RWStructuredBuffer<uint2> blockOffsets; // a workgroup's first output slot in each list
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> counts;        // kCullLists draw counts
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> commands;      // kCullLists * drawCount commands, 5 uints each
[[vk::push_constant]] ConstantBuffer<CullPush> pc;

groupshared uint2 gScan[kCullGroupSize];

struct Bounds
{
    bool inFrustum;   // not wholly outside one clip plane
    bool nearCross;   // a corner in front of the near plane: no screen rectangle
    float minDepth;   // the nearest corner's depth
    float2 lo, hi;    // the corners' NDC rectangle
};

Bounds bounds(DrawRecord d)
{
    const uint base = d.draw.w * (kInstanceGpuStride / 16u);
    const float4 c0 = instances[base], c1 = instances[base + 1u], c2 = instances[base + 2u], c3 = instances[base + 3u];
    Bounds b;
    b.nearCross = false;
    b.minDepth = 1.0;
    b.lo = float2(1e30);
    b.hi = float2(-1e30);
    uint allOut = 63u;
    for (uint k = 0u; k < 8u; ++k)
    {
        const float3 p = float3((k & 1u) != 0u ? d.aabbMax.x : d.aabbMin.x, (k & 2u) != 0u ? d.aabbMax.y : d.aabbMin.y,
                                (k & 4u) != 0u ? d.aabbMax.z : d.aabbMin.z);
        const float4 world = c0 * p.x + c1 * p.y + c2 * p.z + c3;
        const float4 clip = mul(pc.viewProj, world);
        uint outside = 0u;
        if (clip.x < -clip.w) outside |= 1u;
        if (clip.x > clip.w) outside |= 2u;
        if (clip.y < -clip.w) outside |= 4u;
        if (clip.y > clip.w) outside |= 8u;
        if (clip.z < 0.0) outside |= 16u;
        if (clip.z > clip.w) outside |= 32u;
        allOut &= outside;
        if (clip.w <= 1e-6 || clip.z < 0.0)
        {
            b.nearCross = true;
        }
        else
        {
            const float3 ndc = clip.xyz / clip.w;
            b.minDepth = min(b.minDepth, ndc.z);
            b.lo = min(b.lo, ndc.xy);
            b.hi = max(b.hi, ndc.xy);
        }
    }
    // A cascade clamps depth (CSM's depthClampEnable): casters before its near or past its far
    // plane still render, so only its sides cull.
    if (pc.config.y == kCullPhaseShadow) allOut &= 15u;
    b.inFrustum = allOut == 0u;
    return b;
}

// Behind the pyramid of this frame's early depth: the rectangle (a pixel wider each side, for
// the jitter and rounding) read at the level where it spans at most 2x2 texels, whose maximum
// is the farthest depth drawn over it. The draw's nearest point is farther still.
bool occluded(Bounds b)
{
    if (b.nearCross) return false;
    const int2 size = int2(pc.hiz.xy);
    const float2 pmin = (b.lo * 0.5 + 0.5) * float2(size);
    const float2 pmax = (b.hi * 0.5 + 0.5) * float2(size);
    const int2 r0 = clamp(int2(floor(pmin)) - 1, int2(0), size - 1);
    const int2 r1 = clamp(int2(floor(pmax)) + 1, int2(0), size - 1);
    uint level = 0u;
    for (; level + 1u < pc.hiz.z; ++level)
    {
        const int s = int(level + 1u);
        if ((r1.x >> s) - (r0.x >> s) <= 1 && (r1.y >> s) - (r0.y >> s) <= 1) break;
    }
    const int s = int(level + 1u);
    uint w, h, levels;
    hiz.GetDimensions(level, w, h, levels);
    const int2 last = int2(int(w) - 1, int(h) - 1);
    const int2 t0 = min(r0 >> s, last), t1 = min(r1 >> s, last);
    const float far = max(max(hiz.Load(int3(t0.x, t0.y, int(level))), hiz.Load(int3(t1.x, t0.y, int(level)))),
                          max(hiz.Load(int3(t0.x, t1.y, int(level))), hiz.Load(int3(t1.x, t1.y, int(level)))));
    return b.minDepth - 1e-6 > far;
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void classify(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    const uint flags = pc.config.z;
    uint2 c = uint2(0u);
    if (i < pc.config.x)
    {
        const DrawRecord d = draws[i];
        const bool frustum = (flags & kCullFrustum) != 0u;
        const Bounds b = bounds(d);
        const bool inFrustum = !frustum || b.inFrustum;
        bool member;
        if (pc.config.y == kCullPhaseEarly)
        {
            member = frustum && visibility[i] != 0u && inFrustum;
        }
        else if (pc.config.y == kCullPhaseLate)
        {
            const bool drawnEarly = frustum && visibility[i] != 0u && inFrustum;
            const bool visible = inFrustum && !((flags & kCullOcclusion) != 0u && occluded(b));
            member = visible && !drawnEarly;
            visibility[i] = visible ? 1u : 0u;
        }
        else
        {
            member = inFrustum;
        }
        const bool second = (flags & kCullSplitSided) != 0u && (d.info.y & kDrawOneSided) == 0u;
        if (member) c = second ? uint2(0u, 1u) : uint2(1u, 0u);
    }
    // Inclusive scan of the workgroup's memberships (Hillis-Steele, in order).
    gScan[tid.x] = c;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1u; step < kCullGroupSize; step <<= 1u)
    {
        const uint2 add = tid.x >= step ? gScan[tid.x - step] : uint2(0u);
        GroupMemoryBarrierWithGroupSync();
        gScan[tid.x] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint2 exclusive = gScan[tid.x] - c;
    if (i < pc.config.x) local[i] = uint2(exclusive.x | (c.x << 31u), exclusive.y | (c.y << 31u));
    if (tid.x == kCullGroupSize - 1u) blockSums[gid.x] = gScan[tid.x];
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void scanBlocks(uint3 tid : SV_GroupThreadID)
{
    const uint blocks = pc.hiz.w;
    const uint chunk = (blocks + kCullGroupSize - 1u) / kCullGroupSize;
    const uint first = tid.x * chunk;
    const uint end = min(first + chunk, blocks);
    uint2 sum = uint2(0u);
    for (uint b = first; b < end; ++b) sum += blockSums[b];
    gScan[tid.x] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1u; step < kCullGroupSize; step <<= 1u)
    {
        const uint2 add = tid.x >= step ? gScan[tid.x - step] : uint2(0u);
        GroupMemoryBarrierWithGroupSync();
        gScan[tid.x] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 running = gScan[tid.x] - sum;
    for (uint b = first; b < end; ++b)
    {
        blockOffsets[b] = running;
        running += blockSums[b];
    }
    if (tid.x == kCullGroupSize - 1u)
    {
        counts[pc.config.w * 2u] = gScan[tid.x].x;
        counts[pc.config.w * 2u + 1u] = gScan[tid.x].y;
    }
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void scatter(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    const uint n = pc.config.x;
    if (i >= n) return;
    const uint2 l = local[i];
    const uint2 off = blockOffsets[gid.x];
    const DrawRecord d = draws[i];
    for (uint k = 0u; k < 2u; ++k)
    {
        const uint v = k == 0u ? l.x : l.y;
        if ((v >> 31u) == 0u) continue;
        const uint slot = (k == 0u ? off.x : off.y) + (v & 0x7FFFFFFFu);
        const uint at = ((pc.config.w * 2u + k) * n + slot) * 5u;
        commands[at + 0u] = d.draw.y;   // indexCount
        commands[at + 1u] = 1u;         // instanceCount
        commands[at + 2u] = d.draw.x;   // firstIndex
        commands[at + 3u] = d.draw.z;   // vertexOffset
        commands[at + 4u] = i;          // firstInstance: the draw record
    }
}
