// The emissive triangle lights (frontier F3, render/rt/emissive_lights.hpp): one thread a texel of the
// power image's level 0. Light i (i < N) is triangle i - first of the emissive instance whose range
// holds it; it writes the triangle in world space (corners, uvs, material) and its power
//   pi x luminance(emission_color x emission_luminance x the texture's mean over it) x area
// (a Lambertian emitter's). The texture's mean is one sample at the triangle's uv centroid at the mip
// level whose texels match its uv area (RTXDI's light preparation): an estimate, for selection only
// (the path tracer evaluates the texture at the point it samples), so a mostly black emissive texture
// draws few samples. Texels past N hold 0. rt/emissive_reduce.slang sums the levels above.
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<float> vertices;      // Vertex: 25 floats
[[vk::binding(1, 0)]] StructuredBuffer<uint> indices;        // mesh-local
[[vk::binding(2, 0)]] StructuredBuffer<uint4> meshes;        // vertexOffset, indexOffset, indexCount, triangleSlotOffset
[[vk::binding(3, 0)]] StructuredBuffer<uint> triangleSlots;
[[vk::binding(4, 0)]] StructuredBuffer<float4> materials;    // kMaterialRows a material
[[vk::binding(5, 0)]] StructuredBuffer<float4> instances;    // InstanceGpu, kInstanceGpuStride / 16 rows
[[vk::binding(6, 0)]] StructuredBuffer<uint> materialRefs;
[[vk::binding(7, 0)]] StructuredBuffer<uint4> ranges;        // [0] (N, S, levels, R); [1] (analytic power); [2 + r] (slot, first light, triangles, 0)
[[vk::binding(8, 0)]] RWStructuredBuffer<uint4> lights;      // 4 rows a light, as bits: a material index as a float is a denormal, which float paths flush
[[vk::binding(9, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> power;   // level 0
[[vk::binding(0, 1)]] Sampler2D textures[4096];              // set 1: the bindless texture table (BindlessTextureManager)

static const uint kVertexFloats = 25u;
static const float kPi = 3.14159265358979;

float3 position(uint v) { return float3(vertices[v * kVertexFloats], vertices[v * kVertexFloats + 1u], vertices[v * kVertexFloats + 2u]); }
float2 uvOf(uint v) { return float2(vertices[v * kVertexFloats + 9u], vertices[v * kVertexFloats + 10u]); }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint4 header = ranges[0];
    const uint N = header.x, S = header.y, R = header.w;
    const uint i = id.x;
    if (i >= S * S) return;
    const uint2 texel = uint2(i % S, i / S);
    if (i >= N)
    {
        power[texel] = 0.0;
        return;
    }
    // The range holding light i: the last whose first light is at most i.
    uint lo = 0u, hi = R - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1u) >> 1u;
        if (ranges[2u + mid].y <= i) lo = mid;
        else hi = mid - 1u;
    }
    const uint4 range = ranges[2u + lo];
    const uint slot = range.x, primitive = i - range.y;

    const uint stride = kInstanceGpuStride / 16u;
    const uint4 info = asuint(instances[slot * stride + 8u]);   // mesh, refOffset, refCount, flags
    const uint4 mesh = meshes[info.x];
    const uint baseIndex = mesh.y + primitive * 3u;
    const uint v0 = mesh.x + indices[baseIndex], v1 = mesh.x + indices[baseIndex + 1u], v2 = mesh.x + indices[baseIndex + 2u];
    const uint triSlot = triangleSlots[mesh.w + primitive];
    const uint material = triSlot < info.z ? materialRefs[info.y + triSlot] : 0u;

    // World corners: the instance's world matrix as its four columns (rows 0-3).
    const float4 c0 = instances[slot * stride], c1 = instances[slot * stride + 1u];
    const float4 c2 = instances[slot * stride + 2u], c3 = instances[slot * stride + 3u];
    const float3 p0 = position(v0), p1 = position(v1), p2 = position(v2);
    const float3 w0 = (c0 * p0.x + c1 * p0.y + c2 * p0.z + c3).xyz;
    const float3 w1 = (c0 * p1.x + c1 * p1.y + c2 * p1.z + c3).xyz;
    const float3 w2 = (c0 * p2.x + c1 * p2.y + c2 * p2.z + c3).xyz;
    const float2 t0 = uvOf(v0), t1 = uvOf(v1), t2 = uvOf(v2);

    const uint m = material * kMaterialRows;
    float3 emission = materials[m + 6u].rgb * materials[m + 2u].y;
    const uint emissiveTex = asuint(materials[m + 1u].w);
    if (emissiveTex < 4096u)
    {
        uint w, h, levels;
        textures[NonUniformResourceIndex(emissiveTex)].GetDimensions(0u, w, h, levels);
        const float2 e1 = t1 - t0, e2 = t2 - t0;
        const float uvTexels = 0.5 * abs(e1.x * e2.y - e1.y * e2.x) * float(w) * float(h);
        const float lod = clamp(0.5 * log2(max(uvTexels, 1e-8)), 0.0, float(max(levels, 1u) - 1u));
        emission *= textures[NonUniformResourceIndex(emissiveTex)].SampleLevel((t0 + t1 + t2) / 3.0, lod).rgb;
    }
    const float area = 0.5 * length(cross(w1 - w0, w2 - w0));
    const float lum = dot(max(emission, 0.0), float3(0.2126, 0.7152, 0.0722));

    lights[i * 4u] = uint4(asuint(w0), material);
    lights[i * 4u + 1u] = asuint(float4(w1, t0.x));
    lights[i * 4u + 2u] = asuint(float4(w2, t0.y));
    lights[i * 4u + 3u] = asuint(float4(t1, t2));
    power[texel] = isfinite(area * lum) ? kPi * lum * area : 0.0;
}
