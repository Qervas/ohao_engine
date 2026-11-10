// The manifold pass's casters (sub-project 24 step 2, render/rt/manifold_casters.hpp): the triangles of every owned
// caster draw in world space, a cluster a meshlet (render/scene/meshlet_builder: compact patches of at most
// kMeshletMaxTriangles), in groups of kManifoldCluster clusters, each with a box and a cone of its shading normals,
// rebuilt every frame the pass runs. A cluster has 128 triangle slots (its count in its node) and a frame of
// reference c, perpendicular to its cone's axis, that every triangle of it solves in (rt/manifold.slang).
// (Index order made clusters strips round a sphere's rings, their cones a third of the sphere wide: nothing pruned.)
//
// draws (host-written): row 0 (D draws, C * 128, C clusters, G groups); then three rows a draw:
//   (first meshlet, meshlets, vertex offset, instance slot), (first cluster, first group, material, 0), its world
//   bounding sphere (centre, radius as float bits); then a word a material (manifold bits, at 1 + 3 D rows: rows of 4).
// tris: 9 float4 a triangle slot: (p0, c.x), (e1, c.y), (e2, c.z), (n0, 0), (a1, 0), (a2, 0), and its bounds
//   (box min, the cone's half angle), (box max, 0), (the cone's axis, its disc term); normals by the instance's inverse
//   transpose, unnormalised (fillHit's direction).
// nodes: 4 float4 a node, the groups, then the clusters, then a record a draw, then 8 sub-clusters a cluster (16 of
//   its triangles each, in the meshlet's order: the builder grows a meshlet as a compact patch): (box min, the cone's
//   half angle), (box max, triangles as bits), (the cone's axis, its disc term: rt/manifold.slang), (c, 0); a draw's record holds its lobes' alpha
//   bounds in its first row's xy (0 for a lobe that cannot be smooth).
import shared.layout;
import material.openpbr;
import material.material_layers;
import rt.manifold;

[[vk::binding(0, 0)]] StructuredBuffer<float> vertices;      // Vertex: 25 floats
[[vk::binding(1, 0)]] StructuredBuffer<uint4> meshlets;      // MeshletGpu: 3 rows (range, sphere, cone)
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshletTriangles;   // a | b << 8 | c << 16
[[vk::binding(4, 0)]] StructuredBuffer<float4> instances;    // InstanceGpu, kInstanceGpuStride / 16 rows
[[vk::binding(5, 0)]] StructuredBuffer<uint4> draws;
[[vk::binding(6, 0)]] RWStructuredBuffer<float4> tris;
[[vk::binding(7, 0)]] RWStructuredBuffer<float4> nodes;
[[vk::binding(8, 0)]] StructuredBuffer<float4> materials;    // kMaterialRows a material

static const uint kVertexFloats = 25u;
static const uint kSlots = 128u;
static const uint kTriRows = 9u;
static const uint kNodeRows = 4u;
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
        if (draws[2u + 3u * mid][field] <= i) lo = mid;
        else hi = mid - 1u;
    }
    return lo;
}

uint materialBits(uint material)
{
    const uint4 r = draws[1u + 3u * draws[0].x + material / 4u];
    const uint k = material & 3u;
    return k == 0u ? r.x : k == 1u ? r.y : k == 2u ? r.z : r.w;
}

// A unit vector perpendicular to a.
float3 perpendicular(float3 a)
{
    return normalize(cross(a, abs(a.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
}

groupshared float3 gsMin[kSlots];
groupshared float3 gsMax[kSlots];
groupshared float3 gsAxis[kSlots];
groupshared float gsHalf[kSlots];
groupshared uint gsValid[kSlots];
groupshared float3 gsC;

// The node over `count` children in the shared arrays: the box around theirs, the cone's axis the normalised sum of
// theirs, its half angle the largest angle to a child's axis plus the child's half angle (pi past 90 degrees: a cone
// that wide is not convex, and nothing is pruned by it). Returns the axis.
float3 writeNode(uint node, uint count, uint triangles, uint first = 0u, bool useFrame = false, float3 frameIn = float3(0.0))
{
    float3 lo = float3(1e30), hi = float3(-1e30), sum = float3(0.0);
    for (uint k = first; k < first + count; ++k)
    {
        if (gsValid[k] == 0u) continue;
        lo = min(lo, gsMin[k]);
        hi = max(hi, gsMax[k]);
        sum += gsAxis[k];
    }
    const float3 axis = dot(sum, sum) > 1e-20 ? normalize(sum) : float3(0.0, 0.0, 1.0);
    float half = dot(sum, sum) > 1e-20 ? 0.0 : kPi;
    for (uint k = first; k < first + count; ++k)
    {
        if (gsValid[k] == 0u) continue;
        half = max(half, acos(clamp(dot(axis, gsAxis[k]), -1.0, 1.0)) + gsHalf[k]);
    }
    if (half >= 0.5 * kPi) half = kPi;
    // The frame of reference: a cluster's own (perpendicular to its axis), a sub-cluster's its cluster's.
    const float3 frame = useFrame ? frameIn : perpendicular(axis);
    nodes[node * kNodeRows] = float4(lo, half);
    nodes[node * kNodeRows + 1u] = float4(hi, asfloat(triangles));
    nodes[node * kNodeRows + 2u] = float4(axis, manifoldDiscTerm(axis, half, frame));
    nodes[node * kNodeRows + 3u] = float4(frame, 0.0);
    return frame;
}

// A workgroup a cluster (a meshlet): its triangles into their slots, its node, then its frame's c into each triangle.
[shader("compute")]
[numthreads(128, 1, 1)]
void clusters(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint c = gid.x, lane = tid.x;
    const uint4 h = draws[0];
    if (c >= h.z) return;
    const uint d = drawOf(c, 0u);
    const uint4 a = draws[1u + 3u * d], b = draws[2u + 3u * d];
    const uint m = a.x + (c - b.x);
    const uint4 range = meshlets[m * 3u];   // first vertex entry, first triangle entry, vertices, triangles
    const uint count = min(range.w, kSlots);
    gsValid[lane] = 0u;
    const uint slot = c * kSlots + lane;
    ManifoldTri tri;
    if (lane < count)
    {
        const uint packed = meshletTriangles[range.y + lane];
        const uint v0 = a.z + meshletVertices[range.x + (packed & 0xFFu)];
        const uint v1 = a.z + meshletVertices[range.x + ((packed >> 8u) & 0xFFu)];
        const uint v2 = a.z + meshletVertices[range.x + ((packed >> 16u) & 0xFFu)];
        const uint stride = kInstanceGpuStride / 16u;
        tri = manifoldTriangleFrom(instances[a.w * stride], instances[a.w * stride + 1u], instances[a.w * stride + 2u],
                                   instances[a.w * stride + 3u], vertexF3(v0, 0u), vertexF3(v1, 0u), vertexF3(v2, 0u),
                                   vertexF3(v0, 6u), vertexF3(v1, 6u), vertexF3(v2, 6u));
        tri.c = float3(1.0, 0.0, 0.0);
        float3 lo, hi, axis;
        float half;
        manifoldTriangleBounds(tri, lo, hi, axis, half);
        gsMin[lane] = lo;
        gsMax[lane] = hi;
        gsAxis[lane] = axis;
        gsHalf[lane] = half;
        gsValid[lane] = 1u;
        tris[slot * kTriRows + 3u] = float4(tri.n0, 0.0);
        tris[slot * kTriRows + 4u] = float4(tri.a1, 0.0);
        tris[slot * kTriRows + 5u] = float4(tri.a2, 0.0);
        tris[slot * kTriRows + 6u] = float4(lo, half);
        tris[slot * kTriRows + 7u] = float4(hi, 0.0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) gsC = writeNode(h.w + c, count, count);
    GroupMemoryBarrierWithGroupSync();
    // The sub-clusters (in the cluster's frame: one frame for every triangle of it).
    if (lane < 8u && lane * 16u < count)
    {
        const uint sub = h.w + h.z + h.x + c * 8u + lane;
        writeNode(sub, min(16u, count - lane * 16u), min(16u, count - lane * 16u), lane * 16u, true, gsC);
    }
    if (lane < count)
    {
        tris[slot * kTriRows] = float4(tri.p0, gsC.x);
        tris[slot * kTriRows + 1u] = float4(tri.e1, gsC.y);
        tris[slot * kTriRows + 2u] = float4(tri.e2, gsC.z);
        tris[slot * kTriRows + 8u] = float4(gsAxis[lane], manifoldDiscTerm(gsAxis[lane], gsHalf[lane], gsC));
    }
}

// A workgroup a group: kManifoldCluster clusters of one draw; the draw's first group also writes the draw's record.
[shader("compute")]
[numthreads(32, 1, 1)]
void groups(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint g = gid.x, lane = tid.x;
    const uint4 h = draws[0];
    if (g >= h.w) return;
    const uint d = drawOf(g, 1u);
    const uint4 a = draws[1u + 3u * d], b = draws[2u + 3u * d];
    const uint c = (g - b.y) * kManifoldCluster + lane;
    gsValid[lane] = 0u;
    if (lane < kManifoldCluster && c < a.y)
    {
        const uint node = h.w + b.x + c;
        const float4 r0 = nodes[node * kNodeRows], r1 = nodes[node * kNodeRows + 1u], r2 = nodes[node * kNodeRows + 2u];
        gsMin[lane] = r0.xyz;
        gsMax[lane] = r1.xyz;
        gsHalf[lane] = r0.w;
        gsAxis[lane] = r2.xyz;
        gsValid[lane] = asuint(r1.w) > 0u ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u)
    {
        writeNode(g, kManifoldCluster, 0u);
        if (g == b.y)
        {
            // The draw's lobes' alpha bounds from its material's factors (a texture only lowers the roughness), the
            // lobes the host says can be smooth.
            const uint material = b.z;
            const uint row = material * kMaterialRows;
            const float4 row0 = materials[row], row1 = materials[row + 1u], row4 = materials[row + 4u];
            const float4 row5 = materials[row + 5u];
            MaterialInputs mi = defaultMaterialInputs();
            mi.baseColor = row0.rgb;
            mi.baseWeight = row5.z;
            mi.baseMetalness = saturate(row1.y);
            mi.baseDiffuseRoughness = row5.y;
            mi.specularWeight = row4.w;
            mi.specularColor = row4.rgb;
            mi.specularRoughness = saturate(row1.x);
            mi.specularIor = row5.x;
            const uint r = row + kLayerRowFirst;
            NoTextures none;
            applyLayerRows(mi, materials[r], materials[r + 1u], materials[r + 2u], materials[r + 3u], materials[r + 4u],
                           float2(0.0), none);
            const uint t = row + kTransmissionRowFirst;
            applyTransmissionRows(mi, materials[t], materials[t + 1u], materials[t + 2u]);
            float2 alphas = OpenPBRSurface(mi).smoothAlphaBounds(kCausticRough);
            const uint bits = materialBits(material);
            if ((bits & 2u) == 0u) alphas.x = 0.0;
            if ((bits & 4u) == 0u) alphas.y = 0.0;
            nodes[(h.w + h.z + d) * kNodeRows] = float4(alphas, 0.0, 0.0);
        }
    }
}

// Untextured: the factors alone.
struct NoTextures : IMaterialTextures
{
    float4 sample(uint index, float2 uv) { return float4(1.0); }
};
