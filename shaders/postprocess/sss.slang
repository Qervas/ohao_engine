// Subsurface scattering (sub-project 2b spec, "Screen-space passes and order"): a separable
// 7-tap blur of the subsurface pixels' diffuse irradiance E (the lighting pass's second target:
// E, the mask and the diffuse albedo A, clustered_lights' packSubsurfaceDiffuse), on the 16x16
// tiles the G-buffer pass flagged (fragments of materials with `subsurface_weight` > 0). Two
// indirect dispatches, one 16x16 workgroup a tile; no flagged tile, no work. Only the diffuse light
// spreads, as irradiance times the pixel's own albedo (Jimenez et al. 2015's pre- and
// post-scatter split): the specular stays sharp and a uniform surround stays uniform.
//
// blurH   E -> scratch, over the skin tiles and the kSSSRadiusTiles rows above and below them
//         (whatever the vertical kernel reaches): E blurred horizontally (alpha: the mask), 0 off
//         subsurface pixels.
// blurV   scratch -> lit, in place, over the skin tiles: lit + w A (E blurred vertically - E), w the
//         pixel's subsurface_weight.
//
// A tap counts only on a subsurface pixel, weighed by how close its distance from the camera is.
// OpenPBR step 4: the taps sit at the pixel's world-space free path, subsurface_radius x
// subsurface_radius_scale (its material's rows 15-16, through the G-buffer's layers target), weighed
// per channel by Burley's normalized diffusion (Christensen & Burley 2015), r R(r) proportional to
// e^(-r/d) + e^(-r/3d), d the free path over s = 1.85 - A + 7 |A - 0.8|^3 (A subsurface_color); a
// step stays within the kernel's 10 pixels.
import shared.layout;
import material.material_layers;
import includes.lighting.clustered_lights;

[[vk::push_constant]] ConstantBuffer<SSSPush> pc;

[[vk::binding(1, 0)]] Texture2D<float4> gPosition;     // xyz world position
[[vk::binding(2, 0)]] Texture2D<float4> gNormal;       // w < 0: minus the subsurface strength
[[vk::binding(3, 0)]] StructuredBuffer<uint> tiles;   // the G-buffer pass's classification

[[vk::binding(7, 0)]] Texture2D<uint2> gLayers;              // the pixel's material (layersMaterial)
[[vk::binding(8, 0)]] StructuredBuffer<float4> materials;     // kMaterialRows a material

float subsurfaceAt(int2 p) { return max(-gNormal.Load(int3(p, 0)).w, 0.0); }

// The pixel's profile: Burley's d per channel (m) and the step between taps in pixels.
struct Profile
{
    float3 d;
    float step;
};

Profile profileAt(int2 p, float dist)
{
    const uint m = layersMaterial(gLayers.Load(int3(p, 0)));
    const float4 r15 = materials[m * kMaterialRows + kSubsurfaceRowFirst];
    const float4 r16 = materials[m * kMaterialRows + kSubsurfaceRowFirst + 1u];
    const float3 A = saturate(r15.rgb);
    const float3 dA = abs(A - 0.8);
    const float3 s = 1.85 - A + 7.0 * dA * dA * dA;
    Profile pr;
    pr.d = max(r15.w * r16.rgb / s, float3(1e-6));
    // Three steps reach 3 d of the widest channel; a pixel spans dist x params.y.
    const float pixel = max(dist * pc.params.y, 1e-6);
    pr.step = clamp(max(pr.d.x, max(pr.d.y, pr.d.z)) / pixel, 1.0, 10.0);
    return pr;
}

float3 burley(float3 d, float r) { return exp(-r / d) + exp(-r / (3.0 * d)); }
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

[[vk::binding(0, 0)]] Texture2D<uint4> hLit;   // packSubsurfaceDiffuse
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> hOut;

[shader("compute")]
[numthreads(kSSSTile, kSSSTile, 1)]
void blurH(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int2 p;
    if (!pixelOf(kSSSHeaderWords + 2u * kSSSMaxTiles, group.x, local.xy, p)) return;
    const float4 centre = subsurfaceIrradiance(hLit.Load(int3(p, 0)));
    const float skin = subsurfaceAt(p);
    if (skin <= 0.0 || centre.a <= 0.0)
    {
        hOut[p] = float4(0.0);
        return;
    }
    const float d0 = distanceAt(p);
    const Profile pr = profileAt(p, d0);
    const float pixel = d0 * pc.params.y;
    const float3 w0 = burley(pr.d, 0.0);
    float3 sum = centre.rgb * w0, wsum = w0;
    [unroll]
    for (int k = 1; k <= 3; ++k)
    {
        const int off = int(float(k) * pr.step + 0.5);
        const float3 wk = burley(pr.d, float(off) * pixel);
        [unroll]
        for (int s = -1; s <= 1; s += 2)
        {
            const int2 q = int2(clamp(p.x + s * off, 0, int(pc.extent.x) - 1), p.y);
            const float4 dq = subsurfaceIrradiance(hLit.Load(int3(q, 0)));
            const float dd = distanceAt(q) - d0;
            const float3 w = wk * (exp(-dd * dd * 100.0) * dq.a);
            sum += dq.rgb * w;
            wsum += w;
        }
    }
    hOut[p] = float4(sum / max(wsum, float3(1e-6)), 1.0);
}

// ---------------------------------------------------------------------------------------
// vertical

[[vk::binding(5, 0)]] Texture2D<float4> vIn;   // the horizontal pass's output
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> vLit;
[[vk::binding(9, 0)]] Texture2D<uint4> vDiffuse;   // packSubsurfaceDiffuse

[shader("compute")]
[numthreads(kSSSTile, kSSSTile, 1)]
void blurV(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int2 p;
    if (!pixelOf(kSSSHeaderWords + kSSSMaxTiles, group.x, local.xy, p)) return;
    const float skin = subsurfaceAt(p);
    if (skin <= 0.0) return;   // the lit image keeps the pixel
    const uint4 packed = vDiffuse.Load(int3(p, 0));
    const float4 own = subsurfaceIrradiance(packed);
    if (own.a <= 0.0) return;
    const float4 centre = vIn.Load(int3(p, 0));
    const float d0 = distanceAt(p);
    const Profile pr = profileAt(p, d0);
    const float pixel = d0 * pc.params.y;
    const float3 w0 = burley(pr.d, 0.0);
    float3 sum = centre.rgb * w0, wsum = w0;
    [unroll]
    for (int k = 1; k <= 3; ++k)
    {
        const int off = int(float(k) * pr.step + 0.5);
        const float3 wk = burley(pr.d, float(off) * pixel);
        [unroll]
        for (int s = -1; s <= 1; s += 2)
        {
            const int2 q = int2(p.x, clamp(p.y + s * off, 0, int(pc.extent.y) - 1));
            const float4 dq = vIn.Load(int3(q, 0));
            const float dd = distanceAt(q) - d0;
            const float3 w = wk * (exp(-dd * dd * 100.0) * dq.a);
            sum += dq.rgb * w;
            wsum += w;
        }
    }
    const float3 lit = vLit[p].rgb;
    vLit[p] = float4(lit + skin * subsurfaceAlbedo(packed) * (sum / max(wsum, float3(1e-6)) - own.rgb), 1.0);
}
