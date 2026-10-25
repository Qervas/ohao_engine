// The G-buffer's meshlet path (sub-project 2b spec, "Meshlets and mesh shaders"): the draws GPU
// culling put on its meshlet lists (cull/instances.slang), drawn meshlet by meshlet.
//   task   (amplification) one workgroup a kMeshletTaskGroup meshlets of a listed draw: tests
//          them (cull/common.slang: cone, frustum, the Hi-Z pyramid in the late phase) and
//          launches a mesh workgroup for each survivor, in meshlet order
//   mesh   one workgroup a meshlet: its vertices through the draw's instance, its triangles
//   pull   the fallback without VK_EXT_mesh_shader: a vertex shader over the per-meshlet draws
//          classifyMeshlets/scatterMeshlets wrote (firstVertex = the draw << kMeshletVertexShift,
//          firstInstance = the meshlet), pulling each triangle corner through the meshlet
// mesh and pull transform with one function, and both paths cull with one test, so their
// images are identical (test 7). Outputs match core/gbuffer.frag's inputs.
import shared.layout;
import cull.common;

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

// gbuffer.frag's block (the SSS tiles) takes the first 16 bytes.
struct GBufferPush
{
    uint4 fragment;
    MeshletPush m;
};
[[vk::push_constant]] ConstantBuffer<GBufferPush> pc;

static const uint kVertexFloats = 25u;
static const uint kInstanceFloat4s = kInstanceGpuStride / 16u;

struct GVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos : WORLDPOS;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
    [[vk::location(3)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
    [[vk::location(6)]] float4 currentPos : CURRENTPOS;
    [[vk::location(7)]] float4 prevPos : PREVPOS;
    [[vk::location(8)]] nointerpolation uint instance : INSTANCE;
    [[vk::location(9)]] nointerpolation uint materialSlot : MATERIALSLOT;
};

float4x4 matrixAt(uint at)
{
    // The four float4s are the matrix's columns.
    return transpose(float4x4(instances[at], instances[at + 1u], instances[at + 2u], instances[at + 3u]));
}

// An instance's matrices, the same for all of a draw's vertices: a mesh workgroup makes them
// once, the pull shader per vertex, with this one function (so their values are equal).
struct InstanceXform
{
    float4x4 world;
    float4x4 prevWorld;
    float3x3 normalMatrix;   // the world matrix's inverse transpose (gbuffer.vert's)
};

InstanceXform instanceXform(uint slot)
{
    InstanceXform x;
    x.world = matrixAt(slot * kInstanceFloat4s);
    x.prevWorld = matrixAt(slot * kInstanceFloat4s + 4u);
    const float3x3 w3 = float3x3(x.world[0].xyz, x.world[1].xyz, x.world[2].xyz);
    const float3x3 inv = spirv_asm { result:$$float3x3 = OpExtInst glsl450 MatrixInverse $w3 };
    x.normalMatrix = transpose(inv);
    return x;
}

// gbuffer.vert's transform, the same operations: the world position, the normal and tangent
// through the inverse transpose, this and last frame's clip positions.
GVertex shadeVertex(uint vtx, DrawRecord d, InstanceXform x)
{
    const uint b = vtx * kVertexFloats;
    const float3 p = float3(vertices[b], vertices[b + 1u], vertices[b + 2u]);
    const float3 color = float3(vertices[b + 3u], vertices[b + 4u], vertices[b + 5u]);
    const float3 n = float3(vertices[b + 6u], vertices[b + 7u], vertices[b + 8u]);
    const float4 t = float4(vertices[b + 13u], vertices[b + 14u], vertices[b + 15u], vertices[b + 16u]);

    GVertex v;
    const float4 worldPos = mul(x.world, float4(p, 1.0));
    v.worldPos = worldPos.xyz;
    v.normal = normalize(mul(x.normalMatrix, n));
    v.tangent = float4(normalize(mul(x.normalMatrix, t.xyz)), t.w);
    v.color = color;
    v.texCoord = float2(vertices[b + 9u], vertices[b + 10u]);
    v.texCoord1 = float2(vertices[b + 11u], vertices[b + 12u]);
    v.currentPos = mul(camera.viewProj, worldPos);
    v.prevPos = mul(camera.prevViewProj, mul(x.prevWorld, float4(p, 1.0)));
    v.instance = d.draw.w;
    v.materialSlot = d.info.x;
    v.position = v.currentPos;
    return v;
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
