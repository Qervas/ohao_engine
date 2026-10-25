// Screen-space reflections (sub-project 2b spec, "Screen-space passes and order"): the trace,
// at half resolution; postprocess/ssr_resolve.slang upsamples it and puts it in the lit image in
// place of the environment's reflection. Half-resolution pixel q traces for full-resolution
// pixel 2q: a 64-step march along the reflected view ray, in world space, projected into the
// depth buffer; the first sample behind the depth (within `thickness`) returns the lit image
// there. Writes the radiance arriving along the reflection, premultiplied by the confidence c
// in the hit (falling with the march's distance and the roughness, at most 0.8), and c: the
// reflection weight (Fresnel, the split sum, specular occlusion) is the lighting pass's, applied
// once in the resolve. Sky, a miss and rough non-metals write 0 (the environment stays).
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<float4> gPosition;   // xyz world position, w metallic; (0,0,0,0) sky
[[vk::binding(1, 0)]] Texture2D<float4> gNormal;     // xy octahedral normal, z roughness
[[vk::binding(2, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(3, 0)]] Texture2D<float4> litScene;
[[vk::binding(4, 0)]] SamplerState linearSampler;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ssrOut;
[[vk::push_constant]] ConstantBuffer<SSRPush> pc;

static const uint kSteps = 64u;

float3 decodeOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID)
{
    const uint2 q = id.xy;
    if (any(q >= pc.extent.xy)) return;
    const int2 full = int2(pc.extent.zw);
    const int2 p = min(int2(q * 2u), full - 1);
    const float4 position = gPosition.Load(int3(p, 0));
    if (all(position == float4(0.0)))
    {
        ssrOut[q] = float4(0.0);
        return;
    }
    const float3 worldPos = position.xyz;
    const float metallic = position.w;
    const float4 normalSample = gNormal.Load(int3(p, 0));
    const float roughness = normalSample.z;
    if (roughness > 0.7 && metallic < 0.5)   // rough non-metals (skin, fabric): nothing visible
    {
        ssrOut[q] = float4(0.0);
        return;
    }
    const float3 n = decodeOctahedron(normalSample.xy);
    const float3 viewDir = normalize(pc.cameraPos.xyz - worldPos);
    const float3 reflDir = reflect(-viewDir, n);
    const float3 rayStart = worldPos + reflDir * 0.05;
    const float stepSize = pc.cameraPos.w / float(kSteps);

    for (uint i = 0u; i < kSteps; ++i)
    {
        const float4 clip = mul(pc.viewProj, float4(rayStart + reflDir * (stepSize * float(i + 1u)), 1.0));
        if (clip.w < 0.0) break;   // behind the camera
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = ndc.xy * 0.5 + 0.5;
        if (any(uv < float2(0.0)) || any(uv > float2(1.0))) break;
        const int2 sp = min(int2(uv * float2(full)), full - 1);
        const float sceneDepth = depthBuffer.Load(int3(sp, 0));
        if (ndc.z > sceneDepth && ndc.z < sceneDepth + pc.params.x)
        {
            const float confidence = (1.0 - float(i) / float(kSteps)) * (1.0 - roughness) * 0.8;
            ssrOut[q] = float4(litScene.SampleLevel(linearSampler, uv, 0.0).rgb * confidence, confidence);
            return;
        }
    }
    ssrOut[q] = float4(0.0);
}
