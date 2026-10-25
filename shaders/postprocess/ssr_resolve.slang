// Screen-space reflections, the resolve: puts the half-resolution trace (postprocess/ssr.slang)
// into the lit image in place of the environment's reflection, so reflected light is counted
// once. The lighting pass (core/deferred_lighting.frag) added E = W * incoming, the environment
// and RT GI radiance along the reflection times W, the split-sum weight (F0 * A + B) with
// specular occlusion, and wrote E and W beside the lit image. Where the trace hit with
// confidence c and radiance L, the reflection becomes lerp(incoming, L, c) under the same W:
//
//   lit += W * (c * L) - c * E
//
// The trace is upsampled to full resolution from its 4 nearest samples (half-resolution q
// traced for pixel 2q), weighted bilinearly and by how close their surface's distance from the
// camera is to this pixel's (within 5%); where none is the same surface, the closest. A pixel
// no sample reflects anything at is left as lighting wrote it.
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<float4> trace;            // half resolution: rgb c * L, a c
[[vk::binding(1, 0)]] Texture2D<float4> gPosition;        // xyz world position; (0,0,0,0) sky
[[vk::binding(2, 0)]] Texture2D<float4> envSpecular;      // rgb E
[[vk::binding(3, 0)]] Texture2D<float4> specularWeight;   // rgb W
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> lit;
[[vk::push_constant]] ConstantBuffer<SSRResolvePush> pc;

// The distance from the camera of the surface at full-resolution pixel p, 0 for sky.
float surfaceDistance(int2 p)
{
    const float4 position = gPosition.Load(int3(p, 0));
    return all(position == float4(0.0)) ? 0.0 : distance(position.xyz, pc.cameraPos.xyz);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolve(uint3 id : SV_DispatchThreadID)
{
    const int2 pixel = int2(id.xy);
    if (any(id.xy >= pc.extent.xy)) return;
    const int2 full = int2(pc.extent.xy);
    const int2 half = int2(pc.extent.zw);
    const int2 base = pixel >> 1;

    float4 t[4];
    bool any = false;
    for (int i = 0; i < 4; ++i)
    {
        t[i] = trace.Load(int3(min(base + int2(i & 1, i >> 1), half - 1), 0));
        any = any || t[i].a > 0.0;
    }
    if (!any) return;
    const float z = surfaceDistance(pixel);
    if (z <= 0.0) return;

    const float2 f = float2(pixel & 1) * 0.5;
    float4 sum = float4(0.0), best = float4(0.0);
    float wsum = 0.0, bestErr = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        const int2 o = int2(i & 1, i >> 1);
        const float zi = surfaceDistance(min((min(base + o, half - 1)) * 2, full - 1));
        if (zi <= 0.0) continue;
        const float err = abs(zi - z);
        if (err < bestErr) { bestErr = err; best = t[i]; }
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * max(0.0, 1.0 - err / (0.05 * z));
        sum += w * t[i];
        wsum += w;
    }
    const float4 r = wsum > 1e-3 ? sum / wsum : best;
    if (r.a <= 0.0) return;
    const float3 W = specularWeight.Load(int3(pixel, 0)).rgb;
    const float3 E = envSpecular.Load(int3(pixel, 0)).rgb;
    const float4 c = lit[pixel];
    // lit holds E (rounded to half like it), so this stays >= 0 but for rounding; clamp that
    lit[pixel] = float4(max(c.rgb + W * r.rgb - r.a * E, float3(0.0)), c.a);
}
