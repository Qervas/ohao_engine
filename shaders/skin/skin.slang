// Skinning (sub-project 10 spec, "The renderer: skinning, bounds, BLAS, motion vectors"). A skinned
// instance has a mesh of its own in the vertex arena, the size of its base mesh; each frame its pose
// changes:
//   skin    one thread a vertex: the base mesh's bind-pose vertex through this frame's palette
//           (position, normal, tangent; colour, UVs and joints copied) into the instance's range,
//           and through last frame's palette into the copy's boneWeights.xyz, the previous
//           position gbuffer.vert and cull/meshlets.slang take for motion vectors (kDrawSkinned)
//   bounds  one thread a meshlet of the instance's own copies: the sphere around its deformed
//           vertices (the box's centre, the farthest vertex), and no normal cone (cutoff 1: a
//           deforming cluster has none that holds)
// The palette is a matrix a bone, in the mesh's bone order (Model::boneNames): palette[b] =
// joint model matrix x inverse bind. Every sum runs in index order and nothing is shared between
// threads, so the output is the same every run.
import shared.layout;

[[vk::binding(0, 0)]] RWStructuredBuffer<float> vertices;     // the arena, 25 floats a vertex (model.hpp's Vertex)
[[vk::binding(1, 0)]] StructuredBuffer<float4> palettes;       // the frame slot's palettes, a matrix's columns
[[vk::binding(2, 0)]] RWStructuredBuffer<MeshletGpu> meshlets;
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::push_constant]] ConstantBuffer<SkinPush> pc;

static const uint kVertexFloats = 25u;

// The weighted sum of a vertex's bones' matrices (columns), in index order; no weight at all is
// the identity (a vertex no bone holds stays where it is).
float4 blendColumn(uint base, uint4 bones, float4 w, uint column)
{
    float4 c = w.x * palettes[base + bones.x * 4u + column];
    c += w.y * palettes[base + bones.y * 4u + column];
    c += w.z * palettes[base + bones.z * 4u + column];
    c += w.w * palettes[base + bones.w * 4u + column];
    return c;
}

struct Blend
{
    float4 c0, c1, c2, c3;
};

Blend blendOf(uint base, uint4 bones, float4 w)
{
    Blend m;
    if (w.x + w.y + w.z + w.w == 0.0)
    {
        m.c0 = float4(1.0, 0.0, 0.0, 0.0);
        m.c1 = float4(0.0, 1.0, 0.0, 0.0);
        m.c2 = float4(0.0, 0.0, 1.0, 0.0);
        m.c3 = float4(0.0, 0.0, 0.0, 1.0);
        return m;
    }
    m.c0 = blendColumn(base, bones, w, 0u);
    m.c1 = blendColumn(base, bones, w, 1u);
    m.c2 = blendColumn(base, bones, w, 2u);
    m.c3 = blendColumn(base, bones, w, 3u);
    return m;
}

float3 movePoint(Blend m, float3 p) { return m.c0.xyz * p.x + m.c1.xyz * p.y + m.c2.xyz * p.z + m.c3.xyz; }
float3 turnDirection(Blend m, float3 d) { return m.c0.xyz * d.x + m.c1.xyz * d.y + m.c2.xyz * d.z; }

float3 safeNormalize(float3 v) { const float l = length(v); return l > 0.0 ? v / l : v; }

[shader("compute")]
[numthreads(kSkinGroupSize, 1, 1)]
void skin(uint3 id : SV_DispatchThreadID)
{
    const uint v = id.x;
    if (v >= pc.mesh.z) return;
    const uint src = (pc.mesh.x + v) * kVertexFloats;
    const uint dst = (pc.mesh.y + v) * kVertexFloats;

    const float3 p = float3(vertices[src], vertices[src + 1u], vertices[src + 2u]);
    const float3 n = float3(vertices[src + 6u], vertices[src + 7u], vertices[src + 8u]);
    const float4 t = float4(vertices[src + 13u], vertices[src + 14u], vertices[src + 15u], vertices[src + 16u]);
    uint4 bones = uint4(asuint(vertices[src + 17u]), asuint(vertices[src + 18u]), asuint(vertices[src + 19u]),
                        asuint(vertices[src + 20u]));
    const float4 w = float4(vertices[src + 21u], vertices[src + 22u], vertices[src + 23u], vertices[src + 24u]);
    bones = min(bones, uint4(max(pc.palette.z, 1u) - 1u));   // a bad index reads a bone, never past the palette

    const Blend now = blendOf(pc.palette.x * 4u, bones, w);
    const Blend before = blendOf(pc.palette.y * 4u, bones, w);
    const float3 pos = movePoint(now, p);
    const float3 prev = movePoint(before, p);
    const float3 nrm = safeNormalize(turnDirection(now, n));
    const float3 tan = safeNormalize(turnDirection(now, t.xyz));

    vertices[dst] = pos.x;
    vertices[dst + 1u] = pos.y;
    vertices[dst + 2u] = pos.z;
    for (uint k = 3u; k < 6u; ++k) vertices[dst + k] = vertices[src + k];    // colour
    vertices[dst + 6u] = nrm.x;
    vertices[dst + 7u] = nrm.y;
    vertices[dst + 8u] = nrm.z;
    for (uint k = 9u; k < 13u; ++k) vertices[dst + k] = vertices[src + k];   // both UV sets
    vertices[dst + 13u] = tan.x;
    vertices[dst + 14u] = tan.y;
    vertices[dst + 15u] = tan.z;
    vertices[dst + 16u] = t.w;
    for (uint k = 17u; k < 21u; ++k) vertices[dst + k] = vertices[src + k];  // joints
    vertices[dst + 21u] = prev.x;   // boneWeights.xyz: last frame's position (kDrawSkinned)
    vertices[dst + 22u] = prev.y;
    vertices[dst + 23u] = prev.z;
    vertices[dst + 24u] = 0.0;
}

float3 deformed(uint local)
{
    const uint b = (pc.mesh.y + local) * kVertexFloats;
    return float3(vertices[b], vertices[b + 1u], vertices[b + 2u]);
}

[shader("compute")]
[numthreads(kSkinGroupSize, 1, 1)]
void bounds(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.meshlets.y) return;
    const uint at = pc.meshlets.x + id.x;
    MeshletGpu m = meshlets[at];
    float3 lo = float3(3.4e38), hi = float3(-3.4e38);
    for (uint k = 0u; k < m.range.z; ++k)
    {
        const float3 p = deformed(meshletVertices[m.range.x + k]);
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const float3 c = (lo + hi) * 0.5;
    float r = 0.0;
    for (uint k = 0u; k < m.range.z; ++k) r = max(r, length(deformed(meshletVertices[m.range.x + k]) - c));
    m.sphere = float4(c, r);
    m.cone = float4(0.0, 0.0, 1.0, 1.0);   // cutoff 1: never back-facing
    meshlets[at] = m;
}
