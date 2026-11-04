// The manifold pass's casters (sub-project 24 step 2, render/rt/manifold_casters.hpp): the triangles of every owned
// caster draw in world space, a cluster a meshlet (render/scene/meshlet_builder: compact patches of at most
// kMeshletMaxTriangles), in groups of kManifoldCluster clusters, each with a box and a cone of its shading normals,
// rebuilt every frame the pass runs. A cluster has kManifoldClusterSlots triangle slots (its count in its node).
// (Index order made clusters strips round a sphere's rings, their cones a third of the sphere wide: nothing pruned.)
//
// draws (host-written): row 0 (D draws, C * kManifoldClusterSlots, C clusters, G groups); then two rows a draw:
//   (first meshlet, meshlets, vertex offset, instance slot), (first cluster, first group, material, 0); then a word a
//   material (manifoldOwned bits, at 1 + 2 D rows: rows of 4).
// tris: 6 float4 a triangle slot: (p0, 0), (e1, 0), (e2, 0), (n0, 0), (a1, 0), (a2, 0); normals by the instance's
//   inverse transpose, unnormalised (fillHit's direction).
// nodes: 3 float4 a node, the groups then the clusters: (box min, the cone's half angle), (box max, triangles as
//   bits), (the cone's axis, 0).
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<float> vertices;      // Vertex: 25 floats
[[vk::binding(1, 0)]] StructuredBuffer<uint4> meshlets;      // MeshletGpu: 3 rows (range, sphere, cone)
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshletTriangles;   // a | b << 8 | c << 16
[[vk::binding(4, 0)]] StructuredBuffer<float4> instances;    // InstanceGpu, kInstanceGpuStride / 16 rows
[[vk::binding(5, 0)]] StructuredBuffer<uint4> draws;
[[vk::binding(6, 0)]] RWStructuredBuffer<float4> tris;
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> nodes;

static const uint kVertexFloats = 25u;
static const uint kSlots = 128u;   // kManifoldClusterSlots
static const float kPi = 3.14159265358979;

float3 vertexF3(uint v, uint k)
{
    return float3(vertices[v * kVertexFloats + k], vertices[v * kVertexFloats + k + 1u], vertices[v * kVertexFloats + k + 2u]);
}

// The draw holding item i of a prefix sum at field `f` of the second row (0 clusters, 1 groups).
uint drawOf(uint i, uint field)
{
    const uint D = draws[0].x;
    uint lo = 0u, hi = D - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1u) >> 1u;
        if (draws[2u + 2u * mid][field] <= i) lo = mid;
        else hi = mid - 1u;
    }
    return lo;
}

groupshared float3 gsMin[kSlots];
groupshared float3 gsMax[kSlots];
groupshared float3 gsAxis[kSlots];
groupshared float gsHalf[kSlots];
groupshared uint gsValid[kSlots];

// The node over `count` children in the shared arrays: the box around theirs, the cone's axis the normalised sum of
// theirs, its half angle the largest angle to a child's axis plus the child's half angle (pi past 90 degrees: a cone
// that wide is not convex, and nothing is pruned by it).
void writeNode(uint node, uint count, uint triangles)
{
    float3 lo = float3(1e30), hi = float3(-1e30), sum = float3(0.0);
    for (uint k = 0u; k < count; ++k)
    {
        if (gsValid[k] == 0u) continue;
        lo = min(lo, gsMin[k]);
        hi = max(hi, gsMax[k]);
        sum += gsAxis[k];
    }
    const float3 axis = dot(sum, sum) > 1e-20 ? normalize(sum) : float3(0.0, 0.0, 1.0);
    float half = dot(sum, sum) > 1e-20 ? 0.0 : kPi;
    for (uint k = 0u; k < count; ++k)
    {
        if (gsValid[k] == 0u) continue;
        half = max(half, acos(clamp(dot(axis, gsAxis[k]), -1.0, 1.0)) + gsHalf[k]);
    }
    if (half >= 0.5 * kPi) half = kPi;
    nodes[node * 3u] = float4(lo, half);
    nodes[node * 3u + 1u] = float4(hi, asfloat(triangles));
    nodes[node * 3u + 2u] = float4(axis, 0.0);
}

// A workgroup a cluster (a meshlet): its triangles into their slots, and its node.
[shader("compute")]
[numthreads(128, 1, 1)]
void clusters(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint c = gid.x, lane = tid.x;
    const uint4 h = draws[0];
    if (c >= h.z) return;
    const uint d = drawOf(c, 0u);
    const uint4 a = draws[1u + 2u * d], b = draws[2u + 2u * d];
    const uint m = a.x + (c - b.x);
    const uint4 range = meshlets[m * 3u];   // first vertex entry, first triangle entry, vertices, triangles
    const uint count = min(range.w, kSlots);
    gsValid[lane] = 0u;
    const uint slot = c * kSlots + lane;
    if (lane < count)
    {
        const uint packed = meshletTriangles[range.y + lane];
        const uint v0 = a.z + meshletVertices[range.x + (packed & 0xFFu)];
        const uint v1 = a.z + meshletVertices[range.x + ((packed >> 8u) & 0xFFu)];
        const uint v2 = a.z + meshletVertices[range.x + ((packed >> 16u) & 0xFFu)];
        const uint stride = kInstanceGpuStride / 16u;
        const float4 c0 = instances[a.w * stride], c1 = instances[a.w * stride + 1u];
        const float4 c2 = instances[a.w * stride + 2u], c3 = instances[a.w * stride + 3u];
        const float3 p0 = vertexF3(v0, 0u), p1 = vertexF3(v1, 0u), p2 = vertexF3(v2, 0u);
        const float3 w0 = (c0 * p0.x + c1 * p0.y + c2 * p0.z + c3).xyz;
        const float3 w1 = (c0 * p1.x + c1 * p1.y + c2 * p1.z + c3).xyz;
        const float3 w2 = (c0 * p2.x + c1 * p2.y + c2 * p2.z + c3).xyz;
        // The inverse transpose: columns (c1 x c2, c2 x c0, c0 x c1) / det.
        const float3 x = c0.xyz, y = c1.xyz, z = c2.xyz;
        const float det = dot(x, cross(y, z));
        const float3 ix = cross(y, z) / det, iy = cross(z, x) / det, iz = cross(x, y) / det;
        const float3 o0 = vertexF3(v0, 6u), o1 = vertexF3(v1, 6u), o2 = vertexF3(v2, 6u);
        const float3 n0 = ix * o0.x + iy * o0.y + iz * o0.z;
        const float3 n1 = ix * o1.x + iy * o1.y + iz * o1.z;
        const float3 n2 = ix * o2.x + iy * o2.y + iz * o2.z;
        tris[slot * 6u] = float4(w0, 0.0);
        tris[slot * 6u + 1u] = float4(w1 - w0, 0.0);
        tris[slot * 6u + 2u] = float4(w2 - w0, 0.0);
        tris[slot * 6u + 3u] = float4(n0, 0.0);
        tris[slot * 6u + 4u] = float4(n1 - n0, 0.0);
        tris[slot * 6u + 5u] = float4(n2 - n0, 0.0);
        gsMin[lane] = min(w0, min(w1, w2));
        gsMax[lane] = max(w0, max(w1, w2));
        // A triangle's cone: its three normals (any normalised convex combination stays inside while it is under
        // 90 degrees); a degenerate normal makes it the whole sphere.
        const float l0 = length(n0), l1 = length(n1), l2 = length(n2);
        if (l0 > 1e-12 && l1 > 1e-12 && l2 > 1e-12)
        {
            const float3 u0 = n0 / l0, u1 = n1 / l1, u2 = n2 / l2;
            const float3 s = u0 + u1 + u2;
            const float3 ax = dot(s, s) > 1e-20 ? normalize(s) : u0;
            gsAxis[lane] = ax;
            gsHalf[lane] = max(acos(clamp(dot(ax, u0), -1.0, 1.0)),
                               max(acos(clamp(dot(ax, u1), -1.0, 1.0)), acos(clamp(dot(ax, u2), -1.0, 1.0))));
        }
        else
        {
            gsAxis[lane] = float3(0.0, 0.0, 1.0);
            gsHalf[lane] = kPi;
        }
        gsValid[lane] = 1u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) writeNode(h.w + c, count, count);
}

// A workgroup a group: kManifoldCluster clusters of one draw.
[shader("compute")]
[numthreads(32, 1, 1)]
void groups(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint g = gid.x, lane = tid.x;
    const uint4 h = draws[0];
    if (g >= h.w) return;
    const uint d = drawOf(g, 1u);
    const uint4 a = draws[1u + 2u * d], b = draws[2u + 2u * d];
    const uint c = (g - b.y) * kManifoldCluster + lane;
    gsValid[lane] = 0u;
    if (c < a.y)
    {
        const uint node = h.w + b.x + c;
        const float4 r0 = nodes[node * 3u], r1 = nodes[node * 3u + 1u], r2 = nodes[node * 3u + 2u];
        gsMin[lane] = r0.xyz;
        gsMax[lane] = r1.xyz;
        gsHalf[lane] = r0.w;
        gsAxis[lane] = r2.xyz;
        gsValid[lane] = asuint(r1.w) > 0u ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) writeNode(g, kManifoldCluster, 0u);
}
