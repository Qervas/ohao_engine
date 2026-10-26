// GPU culling (sub-project 2b spec, "GPU-driven geometry", "Meshlets and mesh shaders"):
// RenderScene's draw table (one record an instance's submesh) to indirect draw commands, in
// three dispatches a phase.
//   classify     each draw's visibility for the phase (frustum, the Hi-Z pyramid, last frame's
//                visibility), its list, and its offset in its workgroup's output by a
//                workgroup scan
//   scanBlocks   one workgroup: the workgroups' totals to their output offsets, and the counts
//   scatter      each member draw's command at its offset
// Output order is draw order, every run: a prefix sum, never an atomic append. A G-buffer
// phase writes four lists (CullPush.extra.y): instance draws one-sided and double-sided
// (VkDrawIndexedIndirectCommand: face culling on and off), then meshlet draws one-sided and
// double-sided (a task-shader dispatch {groups, 1, 1} with the draw's index in its fourth
// word); a cascade one, every draw on the instance path. List L's commands start at uint
// L * drawCount * 5. firstInstance is the draw's index: the vertex shaders read their draw
// record, and through it the instance, with it.
//
// Without mesh shaders the meshlet draws go through two more dispatches (with scanBlocks
// between): classifyMeshlets tests every meshlet of the phase's meshlet draws (one thread a
// meshlet entry, cull/common.slang's test, as the task shader does), scatterMeshlets writes a
// VkDrawIndirectCommand a surviving meshlet, drawn by cull/meshlets.slang's pull vertex shader.
import shared.layout;
import cull.common;

[[vk::binding(0, 0)]] StructuredBuffer<DrawRecord> draws;
// The instance table read as float4s (kInstanceGpuStride / 16 a slot); the world matrix is
// the first four, its columns.
[[vk::binding(1, 0)]] StructuredBuffer<float4> instances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibility;    // a draw: 1 when the last late phase saw it
[[vk::binding(3, 0)]] RWStructuredBuffer<uint4> local;        // a draw: its offset in its workgroup's lists, bit 31 membership
[[vk::binding(4, 0)]] RWStructuredBuffer<uint4> blockSums;    // a workgroup's member counts
[[vk::binding(5, 0)]] Texture2D<float> hiz;                   // max-depth pyramid; level L texel = 2^(L+1) pixels square
[[vk::binding(6, 0)]] RWStructuredBuffer<uint4> blockOffsets; // a workgroup's first output slot in each list
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> counts;        // kCullCounts command counts
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> commands;      // kCullLists * drawCount commands, 5 uints each
[[vk::binding(9, 0)]] StructuredBuffer<MeshletGpu> meshlets;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint2> meshletLocal;   // a meshlet entry: as `local`, two lists
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> meshletCommands; // kCullMeshletLists * entries commands, 4 uints each
[[vk::push_constant]] ConstantBuffer<CullPush> pc;

groupshared uint4 gScan[kCullGroupSize];

uint instanceBase(DrawRecord d) { return d.draw.w * (kInstanceGpuStride / 16u); }

// The draw's meshlet path under the geometry mode: every draw with meshlets, or (auto) those
// of kMeshletMinTriangles or more.
bool onMeshletPath(DrawRecord d)
{
    if (d.info.w == 0u) return false;
    if (pc.extra.z == kGeometryMeshlets) return true;
    return pc.extra.z == kGeometryAuto && d.draw.y / 3u >= kMeshletMinTriangles;
}

// Inclusive scan of the workgroup's memberships (Hillis-Steele, in order); returns the
// exclusive prefix of `c`.
uint4 workgroupScan(uint t, uint4 c)
{
    gScan[t] = c;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1u; step < kCullGroupSize; step <<= 1u)
    {
        const uint4 add = t >= step ? gScan[t - step] : uint4(0u);
        GroupMemoryBarrierWithGroupSync();
        gScan[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    return gScan[t] - c;
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void classify(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    const uint flags = pc.config.z;
    uint4 c = uint4(0u);
    if (i < pc.config.x)
    {
        const DrawRecord d = draws[i];
        const bool frustum = (flags & kCullFrustum) != 0u;
        const uint base = instanceBase(d);
        const Bounds b = boxBounds(pc.viewProj, instances[base], instances[base + 1u], instances[base + 2u],
                                   instances[base + 3u], d.aabbMin.xyz, d.aabbMax.xyz, pc.config.y == kCullPhaseShadow);
        const bool inFrustum = !frustum || b.inFrustum;
        bool member;
        if (pc.config.y == kCullPhaseEarly)
        {
            member = frustum && visibility[i] != 0u && inFrustum;
        }
        else if (pc.config.y == kCullPhaseLate)
        {
            const bool drawnEarly = frustum && visibility[i] != 0u && inFrustum;
            const bool visible = inFrustum && !((flags & kCullOcclusion) != 0u && occluded(hiz, pc.hiz.xy, pc.hiz.z, b));
            member = visible && !drawnEarly;
            visibility[i] = visible ? 1u : 0u;
        }
        else
        {
            member = inFrustum && (d.info.y & kDrawEmitter) == 0u;   // a light's mesh casts no shadow
        }
        uint k = 0u;
        if (pc.extra.y >= 2u && (d.info.y & kDrawOneSided) == 0u) k = 1u;
        if (pc.extra.y >= 4u && onMeshletPath(d)) k += 2u;
        if (member) c[k] = 1u;
    }
    const uint4 exclusive = workgroupScan(tid.x, c);
    if (i < pc.config.x) local[i] = exclusive | (c << 31u);
    if (tid.x == kCullGroupSize - 1u) blockSums[gid.x] = gScan[tid.x];
}

// Shared by the draw and the meshlet-entry passes: pc.hiz.w blocks, lists extra.x.. (extra.y).
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void scanBlocks(uint3 tid : SV_GroupThreadID)
{
    const uint blocks = pc.hiz.w;
    const uint chunk = (blocks + kCullGroupSize - 1u) / kCullGroupSize;
    const uint first = tid.x * chunk;
    const uint end = min(first + chunk, blocks);
    uint4 sum = uint4(0u);
    for (uint b = first; b < end; ++b) sum += blockSums[b];
    const uint4 exclusive = workgroupScan(tid.x, sum);
    uint4 running = exclusive;
    for (uint b = first; b < end; ++b)
    {
        blockOffsets[b] = running;
        running += blockSums[b];
    }
    if (tid.x == kCullGroupSize - 1u)
    {
        for (uint k = 0u; k < pc.extra.y; ++k) counts[pc.extra.x + k] = gScan[tid.x][k];
    }
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void scatter(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint i = id.x;
    const uint n = pc.config.x;
    if (i >= n) return;
    const uint4 l = local[i];
    const uint4 off = blockOffsets[gid.x];
    const DrawRecord d = draws[i];
    for (uint k = 0u; k < pc.extra.y; ++k)
    {
        const uint v = l[k];
        if ((v >> 31u) == 0u) continue;
        const uint slot = off[k] + (v & 0x7FFFFFFFu);
        const uint at = ((pc.extra.x + k) * n + slot) * 5u;
        if (k >= 2u)
        {
            commands[at + 0u] = (d.info.w + kMeshletTaskGroup - 1u) / kMeshletTaskGroup;   // task groups
            commands[at + 1u] = 1u;
            commands[at + 2u] = 1u;
            commands[at + 3u] = i;    // the draw record (the task shader reads it)
            commands[at + 4u] = 0u;
        }
        else
        {
            commands[at + 0u] = d.draw.y;   // indexCount
            commands[at + 1u] = 1u;         // instanceCount
            commands[at + 2u] = d.draw.x;   // firstIndex
            commands[at + 3u] = d.draw.z;   // vertexOffset
            commands[at + 4u] = i;          // firstInstance: the draw record
        }
    }
}

// ---- the mesh-shader fallback: per-meshlet draws ---------------------------------------------

// The draw holding meshlet entry e: the last whose first entry is at most e (a draw without
// meshlets shares its first entry with the next).
uint drawOfEntry(uint e)
{
    uint lo = 0u, hi = pc.config.w;   // draws [lo, hi); draw 0's first entry is 0
    while (hi - lo > 1u)
    {
        const uint mid = (lo + hi) / 2u;
        if (asuint(draws[mid].aabbMin.w) <= e) lo = mid;
        else hi = mid;
    }
    return lo;
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void classifyMeshlets(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint e = id.x;
    uint4 c = uint4(0u);
    if (e < pc.config.x)
    {
        const uint i = drawOfEntry(e);
        const uint4 l = local[i];   // the phase's draw lists: z the one-sided meshlet list, w the double-sided
        const bool one = (l.z >> 31u) != 0u, two = (l.w >> 31u) != 0u;
        if (one || two)
        {
            const DrawRecord d = draws[i];
            const uint base = instanceBase(d);
            const MeshletView v = meshletView(pc.viewProj, instances[base], instances[base + 1u], instances[base + 2u],
                                              instances[base + 3u], pc.camera.xyz, pc.config.z,
                                              (d.info.y & kDrawOneSided) != 0u, pc.hiz.xy, pc.hiz.z);
            const MeshletGpu m = meshlets[d.info.z + e - asuint(d.aabbMin.w)];
            if (meshletVisible(m, v, hiz)) c = uint4(one ? 1u : 0u, two ? 1u : 0u, 0u, 0u);
        }
    }
    const uint4 exclusive = workgroupScan(tid.x, c);
    if (e < pc.config.x) meshletLocal[e] = exclusive.xy | (c.xy << 31u);
    if (tid.x == kCullGroupSize - 1u) blockSums[gid.x] = gScan[tid.x];
}

[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void scatterMeshlets(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const uint e = id.x;
    const uint n = pc.config.x;
    if (e >= n) return;
    const uint2 l = meshletLocal[e];
    const uint4 off = blockOffsets[gid.x];
    for (uint k = 0u; k < 2u; ++k)
    {
        const uint v = l[k];
        if ((v >> 31u) == 0u) continue;
        const uint i = drawOfEntry(e);
        const DrawRecord d = draws[i];
        const uint meshlet = d.info.z + e - asuint(d.aabbMin.w);
        const uint slot = off[k] + (v & 0x7FFFFFFFu);
        const uint at = ((pc.extra.x - kCullLists + k) * n + slot) * 4u;
        meshletCommands[at + 0u] = meshlets[meshlet].range.w * 3u;   // vertexCount: the meshlet's triangle corners
        meshletCommands[at + 1u] = 1u;                               // instanceCount
        meshletCommands[at + 2u] = i << kMeshletVertexShift;         // firstVertex: the draw record
        meshletCommands[at + 3u] = meshlet;                          // firstInstance: the meshlet
    }
}
