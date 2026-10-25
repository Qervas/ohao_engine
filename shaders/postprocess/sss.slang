// Subsurface scattering (sub-project 2b spec, "Screen-space passes and order"): a separable
// 7-tap blur of the lit image, wider in red than in blue (skin), on the 16x16 tiles the
// G-buffer pass flagged (core/gbuffer.frag: fragments of materials with `subsurface` > 0).
// Two indirect dispatches, one 16x16 workgroup a tile; no flagged tile, no work.
//
// blurH   lit -> scratch, over the skin tiles and the kSSSRadiusTiles rows above and below
//         them (whatever the vertical kernel reaches): skin pixels blurred horizontally,
//         every other pixel copied.
// blurV   scratch -> lit, in place, over the skin tiles: skin pixels blurred vertically.
//
// Each blur mixes its result with the centre by the pixel's subsurface strength (minus the
// G-buffer normal's w) and weights each tap by how close its distance from the camera is.
import shared.layout;

[[vk::push_constant]] ConstantBuffer<SSSPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> gPosition;     // xyz world position
[[vk::binding(2, 0)]] Texture2D<float4> gNormal;       // w < 0: minus the subsurface strength
[[vk::binding(3, 0)]] StructuredBuffer<uint> tiles;   // the G-buffer pass's classification

static const float3 kWeights[4] = {   // centre, then 1, 2, 3 steps out (per channel)
    float3(0.544, 0.786, 0.906), float3(0.178, 0.090, 0.040), float3(0.044, 0.015, 0.006), float3(0.006, 0.002, 0.001)};

float subsurfaceAt(int2 p) { return max(-gNormal.Load(int3(p, 0)).w, 0.0); }
float distanceAt(int2 p) { return distance(gPosition.Load(int3(p, 0)).xyz, pc.cameraPos.xyz); }

// The pixel this thread shades, from the tile list entry of its workgroup; false off-screen.
bool pixelOf(uint listBase, uint group, uint2 local, out int2 p)
{
    const uint tile = tiles[listBase + group];
    p = int2(uint2(tile % pc.extent.z, tile / pc.extent.z) * kSSSTile + local);
    return all(p < int2(pc.extent.xy));
}

// ---------------------------------------------------------------------------------------
// horizontal

[[vk::binding(0, 0)]] Texture2D<float4> hLit;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> hOut;

[shader("compute")]
[numthreads(kSSSTile, kSSSTile, 1)]
void blurH(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int2 p;
    if (!pixelOf(kSSSHeaderWords + 2u * kSSSMaxTiles, group.x, local.xy, p)) return;
    const float4 centre = hLit.Load(int3(p, 0));
    const float skin = subsurfaceAt(p);
    if (skin <= 0.0)
    {
        hOut[p] = centre;
        return;
    }
    const float d0 = distanceAt(p);
    const int step = int(pc.params.x + 0.5);
    float3 sum = centre.rgb * kWeights[0], wsum = kWeights[0];
    [unroll]
    for (int k = 1; k <= 3; ++k)
    {
        [unroll]
        for (int s = -1; s <= 1; s += 2)
        {
            const int2 q = int2(clamp(p.x + s * k * step, 0, int(pc.extent.x) - 1), p.y);
            const float dd = distanceAt(q) - d0;
            const float3 w = kWeights[k] * exp(-dd * dd * 100.0);
            sum += hLit.Load(int3(q, 0)).rgb * w;
            wsum += w;
        }
    }
    hOut[p] = float4(lerp(centre.rgb, sum / max(wsum, float3(0.001)), skin), 1.0);
}

// ---------------------------------------------------------------------------------------
// vertical

[[vk::binding(5, 0)]] Texture2D<float4> vIn;   // the horizontal pass's output
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> vLit;

[shader("compute")]
[numthreads(kSSSTile, kSSSTile, 1)]
void blurV(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int2 p;
    if (!pixelOf(kSSSHeaderWords + kSSSMaxTiles, group.x, local.xy, p)) return;
    const float skin = subsurfaceAt(p);
    if (skin <= 0.0) return;   // the lit image keeps the pixel
    const float4 centre = vIn.Load(int3(p, 0));
    const float d0 = distanceAt(p);
    const int step = int(pc.params.x + 0.5);
    float3 sum = centre.rgb * kWeights[0], wsum = kWeights[0];
    [unroll]
    for (int k = 1; k <= 3; ++k)
    {
        [unroll]
        for (int s = -1; s <= 1; s += 2)
        {
            const int2 q = int2(p.x, clamp(p.y + s * k * step, 0, int(pc.extent.y) - 1));
            const float dd = distanceAt(q) - d0;
            const float3 w = kWeights[k] * exp(-dd * dd * 100.0);
            sum += vIn.Load(int3(q, 0)).rgb * w;
            wsum += w;
        }
    }
    vLit[p] = float4(lerp(centre.rgb, sum / max(wsum, float3(0.001)), skin), 1.0);
}
