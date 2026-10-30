// A candidate triangle's coverage for inline ray queries (sub-project 12, "Materials: alpha
// modes"). An instance with an alpha-tested or blended material is non-opaque in the TLAS
// (VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR, render_scene.cpp), so a ray query that does not
// force opacity sees its triangles as candidates; alphaHit decides each from its own material:
//   opaque   a hit (the instance's other materials made it non-opaque);
//   mask     a hit where coverage >= the cutoff: base colour alpha x the albedo texture's alpha
//            (lod 0) x opacity, as core/gbuffer_mask.frag and the path tracer's anyHit test it;
//   blend    never: a transparent surface casts no shadow and bounces no GI in the hybrid frame.
// Set 1 is RenderScene's tables (render/rt/rt_alpha_tables), set 2 the bindless textures.
// import rt.alpha_test;
import shared.layout;
import material.openpbr;

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

// What an inline ray query's committed hit is made of (OpenPBR step 1, T5): the hit triangle's
// material resolved at its uv, as the G-buffer and the path tracer's closest hit resolve it
// (base colour and its texture, roughness and metalness and their texture, the KHR specular
// textures), and its emission (emission_color x emission_luminance, times the emissive texture).
struct HitMaterial
{
    MaterialInputs inputs;
    float3 emission;
};

float4 sampleAt(uint index, float2 uv)
{
    return atTextures[NonUniformResourceIndex(index)].SampleLevel(uv, 0.0);
}

HitMaterial hitMaterial(uint slot, uint primitive, float2 bary)
{
    const uint stride = kInstanceGpuStride / 16u;
    const uint4 info = asuint(atInstances[slot * stride + 8u]);   // mesh, refOffset, refCount, flags
    const uint4 mesh = atMeshes[info.x];
    const uint triSlot = atTriangleSlots[mesh.w + primitive];
    const uint material = triSlot < info.z ? atMaterialRefs[info.y + triSlot] : 0u;
    const uint m = material * kMaterialRows;
    const float4 row0 = atMaterials[m], row1 = atMaterials[m + 1u], row2 = atMaterials[m + 2u];
    const float4 row4 = atMaterials[m + 4u], row5 = atMaterials[m + 5u], row6 = atMaterials[m + 6u];
    const uint base = mesh.y + primitive * 3u;
    const float2 uv = vertexUv(mesh.x + atIndices[base]) * (1.0 - bary.x - bary.y) +
                      vertexUv(mesh.x + atIndices[base + 1u]) * bary.x + vertexUv(mesh.x + atIndices[base + 2u]) * bary.y;
    HitMaterial h;
    h.inputs.baseColor = row0.rgb;
    if (asuint(row0.w) < 4096u) h.inputs.baseColor *= sampleAt(asuint(row0.w), uv).rgb;
    float roughness = row1.x, metalness = row1.y;
    if (asuint(row2.x) < 4096u)
    {
        const float4 rm = sampleAt(asuint(row2.x), uv);
        roughness *= rm.g;
        metalness *= rm.b;
    }
    h.inputs.baseWeight = row5.z;
    h.inputs.baseMetalness = saturate(metalness);
    h.inputs.baseDiffuseRoughness = row5.y;
    h.inputs.specularWeight = row4.w;
    if (asuint(row5.w) < 4096u) h.inputs.specularWeight *= sampleAt(asuint(row5.w), uv).a;
    h.inputs.specularColor = row4.rgb;
    if (asuint(row6.w) < 4096u) h.inputs.specularColor *= sampleAt(asuint(row6.w), uv).rgb;
    h.inputs.specularRoughness = saturate(roughness);
    h.inputs.specularIor = row5.x;
    h.emission = row6.rgb * row2.y;
    if (asuint(row1.w) < 4096u) h.emission *= sampleAt(asuint(row1.w), uv).rgb;
    return h;
}
