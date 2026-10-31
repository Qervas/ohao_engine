// The G-buffer's meshlet path (sub-project 2b spec, "Meshlets and mesh shaders"): the draws GPU
// culling put on its meshlet lists (cull/instances.slang), drawn meshlet by meshlet.
//   task   (amplification) one workgroup a kMeshletTaskGroup meshlets of a listed draw: tests
//          them (cull/common.slang: cone, frustum, the Hi-Z pyramid in the late phase) and
//          launches a mesh workgroup for each survivor, in meshlet order
//   mesh   one workgroup a meshlet: its vertices through the draw's instance, its triangles
//   pull   the fallback without VK_EXT_mesh_shader: a vertex shader over the per-meshlet draws
//          classifyMeshlets/scatterMeshlets wrote (firstVertex = the draw << kMeshletVertexShift,
//          firstInstance = the meshlet), pulling each triangle corner through the meshlet
// mesh and pull transform with the instance path's function (includes/scene/gbuffer_vertex.slang),
// and both paths cull with one test, so their images are identical (test 7).
import shared.layout;
import cull.common;
import includes.scene.gbuffer_vertex;

// Set 1 "scene" (GBufferPass): the instance table as float4s (kInstanceGpuStride / 16 a slot:
// world matrix columns 0-3, last frame's 4-7), the camera, the draw table, the vertex arena as
// floats (25 a vertex: model.hpp's Vertex), the meshlet arenas, GPU culling's commands, the
// pyramid.
[[vk::binding(0, 1)]] StructuredBuffer<float4> instances;
struct CameraData
{
    float4x4 viewProj;
    float4x4 prevViewProj;
};
[[vk::binding(3, 1)]] ConstantBuffer<CameraData> camera;
[[vk::binding(5, 1)]] StructuredBuffer<DrawRecord> draws;
[[vk::binding(6, 1)]] StructuredBuffer<float> vertices;
[[vk::binding(7, 1)]] StructuredBuffer<MeshletGpu> meshlets;
[[vk::binding(8, 1)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(9, 1)]] StructuredBuffer<uint> meshletTriangles;
[[vk::binding(10, 1)]] StructuredBuffer<uint> commands;
[[vk::binding(11, 1)]] Texture2D<float> hiz;

// The G-buffer fragment shader's block (the SSS tiles) takes the first 16 bytes.
struct GBufferPush
{
    uint4 fragment;
    MeshletPush m;
};
[[vk::push_constant]] ConstantBuffer<GBufferPush> pc;

static const uint kVertexFloats = 25u;
static const uint kInstanceFloat4s = kInstanceGpuStride / 16u;

float4x4 matrixAt(uint at)
{
    // The four float4s are the matrix's columns.
    return transpose(float4x4(instances[at], instances[at + 1u], instances[at + 2u], instances[at + 3u]));
}

// An instance's matrices from the instance table's float4s (a mesh workgroup makes them once, the
// pull shader per vertex).
InstanceXform instanceXform(uint slot)
{
    return instanceXform(matrixAt(slot * kInstanceFloat4s), matrixAt(slot * kInstanceFloat4s + 4u));
}

// A vertex of the arena through the draw's instance.
GVertex shadeVertex(uint vtx, DrawRecord d, InstanceXform x)
{
    const uint b = vtx * kVertexFloats;
    const float3 p = float3(vertices[b], vertices[b + 1u], vertices[b + 2u]);
    // A skinned instance's own mesh keeps last frame's position in boneWeights.xyz.
    const float3 prevLocal = (d.info.y & kDrawSkinned) != 0u
                                 ? float3(vertices[b + 21u], vertices[b + 22u], vertices[b + 23u]) : p;
    return transformVertex(x, camera.viewProj, camera.prevViewProj, p,
                           float3(vertices[b + 3u], vertices[b + 4u], vertices[b + 5u]),
                           float3(vertices[b + 6u], vertices[b + 7u], vertices[b + 8u]),
                           float4(vertices[b + 13u], vertices[b + 14u], vertices[b + 15u], vertices[b + 16u]),
                           float2(vertices[b + 9u], vertices[b + 10u]), float2(vertices[b + 11u], vertices[b + 12u]),
                           prevLocal, d.draw.w, d.info.x);
}

// ---- mesh shaders ------------------------------------------------------------------------------

struct Payload
{
    uint draw;
    uint meshlet[kMeshletTaskGroup];
};
groupshared Payload gPayload;
groupshared uint gKeep[kMeshletTaskGroup];
groupshared InstanceXform gXform;   // the mesh workgroup's draw's

[shader("amplification")]
[numthreads(kMeshletTaskGroup, 1, 1)]
void task(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint drawId : SV_DrawIndex)
{
    // The list's command drawId names its draw record (cull/instances.slang scatter).
    const uint drawIndex = commands[pc.m.config.x + drawId * 5u + 3u];
    const DrawRecord d = draws[drawIndex];
    const uint t = tid.x;
    const uint local = gid.x * kMeshletTaskGroup + t;
    bool keep = false;
    if (local < d.info.w)
    {
        const uint base = d.draw.w * kInstanceFloat4s;
        const MeshletView view = meshletView(camera.viewProj, instances[base], instances[base + 1u], instances[base + 2u],
                                             instances[base + 3u], pc.m.camera.xyz, pc.m.config.y,
                                             (d.info.y & kDrawOneSided) != 0u, pc.m.hiz.xy, pc.m.config.z);
        keep = meshletVisible(meshlets[d.info.z + local], view, hiz);
    }
    // Compaction in meshlet order: an inclusive scan of the survivors.
    gKeep[t] = keep ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1u; step < kMeshletTaskGroup; step <<= 1u)
    {
        const uint add = t >= step ? gKeep[t - step] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gKeep[t] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (keep) gPayload.meshlet[gKeep[t] - 1u] = d.info.z + local;
    if (t == 0u) gPayload.draw = drawIndex;
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(gKeep[kMeshletTaskGroup - 1u], 1, 1, gPayload);
}

// A thread a vertex (64; 32 threads with two vertices each measured slower: helmet room 1080p
// gbuffer.early 0.232 ms against 0.218), each writing two of the triangles at most.
static const uint kMeshThreads = kMeshletMaxVertices;

[shader("mesh")]
[numthreads(kMeshThreads, 1, 1)]
[outputtopology("triangle")]
void mesh(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID, in payload Payload p,
          out vertices GVertex verts[kMeshletMaxVertices], out indices uint3 tris[kMeshletMaxTriangles])
{
    const MeshletGpu m = meshlets[p.meshlet[gid.x]];
    const DrawRecord d = draws[p.draw];
    const uint t = tid.x;
    if (t == 0u) gXform = instanceXform(d.draw.w);
    GroupMemoryBarrierWithGroupSync();
    SetMeshOutputCounts(m.range.z, m.range.w);
    for (uint k = t; k < m.range.z; k += kMeshThreads)
        verts[k] = shadeVertex(d.draw.z + meshletVertices[m.range.x + k], d, gXform);
    for (uint k = t; k < m.range.w; k += kMeshThreads)
    {
        const uint packed = meshletTriangles[m.range.y + k];
        tris[k] = uint3(packed & 0xFFu, (packed >> 8u) & 0xFFu, (packed >> 16u) & 0xFFu);
    }
}

// ---- the fallback ------------------------------------------------------------------------------

[shader("vertex")]
GVertex pull(uint vertexIndex : SV_VulkanVertexID, uint instanceIndex : SV_VulkanInstanceID)
{
    const DrawRecord d = draws[vertexIndex >> kMeshletVertexShift];
    const uint corner = vertexIndex & ((1u << kMeshletVertexShift) - 1u);
    const MeshletGpu m = meshlets[instanceIndex];
    const uint packed = meshletTriangles[m.range.y + corner / 3u];
    const uint local = (packed >> (8u * (corner % 3u))) & 0xFFu;
    return shadeVertex(d.draw.z + meshletVertices[m.range.x + local], d, instanceXform(d.draw.w));
}
