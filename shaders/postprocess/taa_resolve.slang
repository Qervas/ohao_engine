// TAA's resolve, which is also the upscaler (render.settings renderScale): the current frame may be smaller than the
// history and the output.
// - Each frame's samples sit at its pixel centres moved by that frame's jitter, so over the jitter sequence they
//   cover the output's pixels. An output pixel takes this frame's nearest sample by how close it fell to the pixel's
//   centre (a Gaussian of the distance in output pixels: Karis 2014's temporal upsample, as Unreal's TAAU), and
//   keeps its history otherwise.
// - The history is reprojected by the motion vectors less this frame's jitter (an output pixel stands still), sampled
//   Catmull-Rom (four bilinear taps), and clipped toward the sample's 3x3 neighbourhood's mean +- 1 sigma in YCoCg
//   (variance clipping, Salvi 2016).
// - Motion and a transparent's coverage weigh the history less.

[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
[[vk::binding(2, 0)]] Sampler2D velocityBuffer;
[[vk::binding(3, 0)]] Sampler2D depthBuffer;
// The transparent pass's coverage (R8, sub-project 12, OPEN ruling 15): no velocity is written for
// transparents, so where they cover a pixel its history weighs less (a reactive mask).
[[vk::binding(4, 0)]] Sampler2D transparencyMask;

struct TAAParams
{
    float2 texelSize;      // the output's (the history's)
    float2 inputTexelSize; // the current frame's
    float2 jitterUv;       // where this frame's jitter moved the image, in uv
    float blendFactor;     // the history's weight (higher: more history)
    float motionScale;     // how fast motion takes it toward 0.5
    uint flags;            // bit 0 the motion vectors, bit 1 variance clipping, bit 2 the transparency mask bound
    float sharpness;       // the sample weight's Gaussian: 1 / (2 sigma^2), in output pixels
    float pad;
};
[[vk::push_constant]] ConstantBuffer<TAAParams> params;

float3 RGBToYCoCg(float3 rgb)
{
    return float3(
         0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
         0.5 * rgb.r                 - 0.5 * rgb.b,
        -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b);
}

float3 YCoCgToRGB(float3 YCoCg)
{
    return float3(
        YCoCg.x + YCoCg.y - YCoCg.z,
        YCoCg.x           + YCoCg.z,
        YCoCg.x - YCoCg.y - YCoCg.z);
}

// prevSample moved toward the box's centre until it is inside the box.
float3 clipToAABB(float3 aabbMin, float3 aabbMax, float3 prevSample)
{
    const float3 pClip = 0.5 * (aabbMax + aabbMin);
    const float3 eClip = 0.5 * (aabbMax - aabbMin) + 0.001;
    const float3 vClip = prevSample - pClip;
    const float3 vUnit = vClip / eClip;
    const float3 aUnit = abs(vUnit);
    const float maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
    if (maUnit > 1.0)
        return pClip + vClip / maUnit;
    return prevSample;
}

// The history at uv, Catmull-Rom through four bilinear taps.
float3 sampleHistory(float2 uv)
{
    const float2 samplePos = uv / params.texelSize - 0.5;
    float2 texPos = floor(samplePos);
    const float2 f = samplePos - texPos;
    texPos = (texPos + 0.5) * params.texelSize;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 s0 = w0 + w1;
    const float2 s1 = w2 + w3;
    const float2 f0 = w1 / s0;
    const float2 f1 = w3 / s1;
    const float2 t0 = texPos - params.texelSize * (1.0 - f0);
    const float2 t1 = texPos + params.texelSize * (1.0 + f1);

    const float3 result =
        (historyFrame.Sample(float2(t0.x, t0.y)).rgb * s0.x +
         historyFrame.Sample(float2(t1.x, t0.y)).rgb * s1.x) * s0.y +
        (historyFrame.Sample(float2(t0.x, t1.y)).rgb * s0.x +
         historyFrame.Sample(float2(t1.x, t1.y)).rgb * s1.x) * s1.y;
    return max(result, float3(0.0));
}

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    // The current frame's texel this pixel reads, and how much of the pixel that sample stands for.
    const float2 q = (uv + params.jitterUv) / params.inputTexelSize;   // in the jittered image's pixels
    const float2 c = floor(q) + 0.5;
    const float2 d = (c - q) * params.inputTexelSize / params.texelSize;   // in output pixels
    const float2 texelUv = c * params.inputTexelSize;
    const float sampleWeight = exp(-dot(d, d) * params.sharpness);
    const float3 currentColor = currentFrame.Sample(texelUv).rgb;

    float2 velocity = float2(0.0);
    if ((params.flags & 1u) != 0u)
        velocity = velocityBuffer.Sample(texelUv).rg;

    // The motion vectors run from last frame's unjittered position to this frame's jittered one; an output pixel
    // stands still, so the jitter comes out (left in, the history is resampled a jitter away every frame: a blur).
    velocity -= params.jitterUv;
    const float2 historyUV = uv - velocity;
    const bool validHistory = historyUV.x >= 0.0 && historyUV.x <= 1.0 && historyUV.y >= 0.0 && historyUV.y <= 1.0;
    if (!validHistory || params.blendFactor <= 0.0)   // no history: this frame, bilinear
        return float4(currentFrame.Sample(uv + params.jitterUv).rgb, 1.0);

    float3 historyColor = sampleHistory(historyUV);

    float3 m1 = float3(0.0);
    float3 m2 = float3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float3 c = currentFrame.Sample(texelUv + float2(x, y) * params.inputTexelSize).rgb;
            c = RGBToYCoCg(c);
            m1 += c;
            m2 += c * c;
        }
    }
    m1 /= 9.0;
    m2 /= 9.0;

    const float3 sigma = sqrt(max(m2 - m1 * m1, float3(0.0)));
    const float velocityMag = length(velocity);
    const float motionWeight = clamp(velocityMag * params.motionScale, 0.0, 1.0);
    const float varClipGamma = 1.0;
    const float3 aabbMin = m1 - varClipGamma * sigma;
    const float3 aabbMax = m1 + varClipGamma * sigma;

    float3 historyYCoCg = RGBToYCoCg(historyColor);
    historyYCoCg = clipToAABB(aabbMin, aabbMax, historyYCoCg);
    historyColor = YCoCgToRGB(historyYCoCg);

    float blend = lerp(params.blendFactor, 0.5, motionWeight);
    // Under a transparent (particles, tracers, a blended pane): toward the current frame. A pixel
    // no transparent covers keeps its blend exactly.
    const float transparency = (params.flags & 4u) != 0u ? transparencyMask.Sample(texelUv).r : 0.0;
    if (transparency > 0.0) blend = lerp(blend, 0.2, transparency);

    // The sample counts for as much of this pixel as it covers.
    blend = 1.0 - (1.0 - blend) * sampleWeight;

    return float4(lerp(currentColor, historyColor, blend), 1.0);
}
