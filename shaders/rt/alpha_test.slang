// A candidate triangle's coverage for inline ray queries (sub-project 12, "Materials: alpha
// modes"). An instance with an alpha-tested or blended material is non-opaque in the TLAS
// (VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR, render_scene.cpp), so a ray query that does not
// force opacity sees its triangles as candidates; alphaHit decides each from its own material:
//   opaque   a hit (the instance's other materials made it non-opaque);
//   mask     a hit where coverage >= the cutoff: base colour alpha x the albedo texture's alpha
//            (lod 0) x opacity, as core/gbuffer_mask.frag and pt_anyhit test it;
//   blend    never: a transparent surface casts no shadow and bounces no GI in the hybrid frame.
// Set 1 is RenderScene's tables (render/rt/rt_alpha_tables), set 2 the bindless textures.
// import rt.alpha_test;
import shared.layout;

[[vk::binding(0, 1)]] StructuredBuffer<float4> atInstances;   // kInstanceGpuStride / 16 a slot
[[vk::binding(1, 1)]] StructuredBuffer<uint> atMaterialRefs;
[[vk::binding(2, 1)]] StructuredBuffer<float4> atMaterials;   // kMaterialRows a material
[[vk::binding(3, 1)]] StructuredBuffer<uint4> atMeshes;       // vertexOffset, indexOffset, indexCount, triangleSlotOffset
[[vk::binding(4, 1)]] StructuredBuffer<uint> atTriangleSlots;
[[vk::binding(5, 1)]] StructuredBuffer<uint> atIndices;       // mesh-local
[[vk::binding(6, 1)]] StructuredBuffer<float> atVertices;     // Vertex: 25 floats, uv at 9
// The bindless table at its size (BindlessTextureManager's 4096): Slang decorates an unsized
// resource array with an ArrayStride that spirv-val rejects (VUID-StandaloneSpirv-None-10684).
[[vk::binding(0, 2)]] Sampler2D atTextures[4096];

static const uint kVertexFloats = 25u;
static const uint kVertexUv = 9u;

float2 vertexUv(uint v)
{
    const uint b = v * kVertexFloats + kVertexUv;
    return float2(atVertices[b], atVertices[b + 1u]);
}

// Whether the candidate (TLAS custom index = the instance slot, its primitive, its
// barycentrics) is a hit.
bool alphaHit(uint slot, uint primitive, float2 bary)
{
    const uint stride = kInstanceGpuStride / 16u;
    const uint4 info = asuint(atInstances[slot * stride + 8u]);   // mesh, refOffset, refCount, flags
    const uint4 mesh = atMeshes[info.x];
    const uint triSlot = atTriangleSlots[mesh.w + primitive];
    const uint material = triSlot < info.z ? atMaterialRefs[info.y + triSlot] : 0u;
    const float4 row3 = atMaterials[material * kMaterialRows + 3u];   // mode, cutoff, opacity, base alpha
    const uint mode = uint(row3.x + 0.5);
    if (mode == kAlphaOpaque) return true;
    if (mode == kAlphaBlend) return false;
    float alpha = row3.w * row3.z;
    const uint albedoTex = asuint(atMaterials[material * kMaterialRows].w);
    if (albedoTex != 0xFFFFFFFFu)
    {
        const uint base = mesh.y + primitive * 3u;
        const float2 uv = vertexUv(mesh.x + atIndices[base]) * (1.0 - bary.x - bary.y) +
                          vertexUv(mesh.x + atIndices[base + 1u]) * bary.x + vertexUv(mesh.x + atIndices[base + 2u]) * bary.y;
        alpha *= atTextures[NonUniformResourceIndex(albedoTex)].SampleLevel(uv, 0.0).a;
    }
    return alpha >= row3.y;
}
