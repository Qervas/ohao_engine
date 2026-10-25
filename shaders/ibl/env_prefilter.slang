// The environment for the deferred frame's image-based lighting (render/deferred/env_prefilter):
// built once per environment map, four compute entry points over one descriptor set layout.
// Every image is equirectangular, mapped as the lighting pass maps a direction:
// u = atan2(d.z, d.x) / 2pi + 0.5, v = asin(d.y) / pi + 0.5.
//
// copySource  The HDR environment (RGBA32F) into level 0 of the source chain (RGBA16F).
// downsample  Each further chain level: the 2x2 mean of the level above.
// prefilter   One level of the prefiltered environment: for the direction of each texel, the
//             GGX lobe at roughness params.x (normal = view = reflection, the split-sum
//             assumption) integrated with size.z Hammersley samples, each read from the chain
//             at the mip its solid angle covers (filtered importance sampling), so a few
//             hundred samples give a smooth result without fireflies from bright spots.
// irradiance  The cosine-weighted mean of the incoming radiance around each texel's
//             direction (diffuse irradiance over pi), the same way with a cosine lobe.
//
// The sample sequence is fixed (no seed): the output depends only on the environment.
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<float4> inImage;      // copySource: the environment; downsample: the level above
[[vk::binding(1, 0)]] Texture2D<float4> chain;        // prefilter, irradiance: the source chain, every mip
[[vk::binding(2, 0)]] SamplerState chainSampler;      // linear, mips linear, u repeat, v clamped
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outImage;
[[vk::push_constant]] ConstantBuffer<EnvPrefilterPush> pc;

static const float kPi = 3.14159265;
static const float kTwoPi = 6.2831853;

float2 dirToUv(float3 d)
{
    return float2(atan2(d.z, d.x) / kTwoPi + 0.5, asin(clamp(d.y, -1.0, 1.0)) / kPi + 0.5);
}

float3 uvToDir(float2 uv)
{
    const float phi = (uv.x - 0.5) * kTwoPi;
    const float theta = (uv.y - 0.5) * kPi;
    return float3(cos(theta) * cos(phi), sin(theta), cos(theta) * sin(phi));
}

float2 hammersley(uint i, uint n)
{
    return float2((float(i) + 0.5) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}

void basis(float3 n, out float3 t, out float3 b)
{
    const float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// The chain at direction d, at the mip whose texels cover `sampleSolidAngle`. A texel of the
// base level covers (2pi / w)(pi / h) cos(latitude) steradians.
float3 chainAt(float3 d, float sampleSolidAngle)
{
    const float texel = (kTwoPi / pc.params.y) * (kPi / pc.params.z) * max(sqrt(saturate(1.0 - d.y * d.y)), 1e-4);
    const float lod = clamp(0.5 * log2(sampleSolidAngle / texel) + 1.0, 0.0, float(pc.size.w - 1u));
    return chain.SampleLevel(chainSampler, dirToUv(d), lod).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void copySource(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    outImage[id.xy] = float4(inImage.Load(int3(id.xy, 0)).rgb, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    uint2 above;
    inImage.GetDimensions(above.x, above.y);
    const uint2 p = id.xy * 2u;
    const float3 sum = inImage.Load(int3(min(p, above - 1u), 0)).rgb +
                       inImage.Load(int3(min(p + uint2(1, 0), above - 1u), 0)).rgb +
                       inImage.Load(int3(min(p + uint2(0, 1), above - 1u), 0)).rgb +
                       inImage.Load(int3(min(p + uint2(1, 1), above - 1u), 0)).rgb;
    outImage[id.xy] = float4(sum * 0.25, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilter(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    const float3 n = uvToDir((float2(id.xy) + 0.5) / float2(pc.size.xy));
    float3 t, b;
    basis(n, t, b);
    const float a = pc.params.x * pc.params.x;   // GGX alpha
    const float a2 = a * a;
    const uint samples = pc.size.z;
    float3 sum = float3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < samples; ++i)
    {
        const float2 xi = hammersley(i, samples);
        const float phi = kTwoPi * xi.y;
        const float cosH = sqrt((1.0 - xi.x) / (1.0 + (a2 - 1.0) * xi.x));
        const float sinH = sqrt(saturate(1.0 - cosH * cosH));
        const float3 h = normalize(t * (sinH * cos(phi)) + b * (sinH * sin(phi)) + n * cosH);
        const float3 l = 2.0 * dot(n, h) * h - n;   // view = normal
        const float nDotL = dot(n, l);
        if (nDotL <= 0.0) continue;
        // pdf(l) = D(h) cosH / (4 (v.h)) = D(h) / 4 when v = n.
        const float denom = cosH * cosH * (a2 - 1.0) + 1.0;
        const float d = a2 / (kPi * denom * denom);
        const float sampleSolidAngle = 1.0 / (float(samples) * max(d * 0.25, 1e-6));
        sum += chainAt(l, sampleSolidAngle) * nDotL;
        wsum += nDotL;
    }
    outImage[id.xy] = float4(wsum > 0.0 ? sum / wsum : float3(0.0), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void irradiance(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    const float3 n = uvToDir((float2(id.xy) + 0.5) / float2(pc.size.xy));
    float3 t, b;
    basis(n, t, b);
    const uint samples = pc.size.z;
    float3 sum = float3(0.0);
    for (uint i = 0u; i < samples; ++i)
    {
        const float2 xi = hammersley(i, samples);
        const float r = sqrt(xi.x);
        const float phi = kTwoPi * xi.y;
        const float cosL = sqrt(max(0.0, 1.0 - xi.x));
        const float3 l = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * cosL);
        // Cosine sampling: pdf = cos / pi; the mean of the samples is the irradiance over pi.
        const float sampleSolidAngle = 1.0 / (float(samples) * max(cosL / kPi, 1e-4));
        sum += chainAt(l, sampleSolidAngle);
    }
    outImage[id.xy] = float4(sum / float(samples), 1.0);
}
