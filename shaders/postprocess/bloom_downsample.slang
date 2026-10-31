// Bloom's downsample: a 13-tap filter (Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare", SIGGRAPH 2014), its five 2x2 groups weighed 1/8 each and 1/2 for the centre;
// the first pass weighs each group by its Karis weight 1 / (max channel + 1) and renormalises, so a
// small bright source (a light's mesh, a highlight) cannot flood the pyramid.

[[vk::binding(0, 0)]] Sampler2D inputTexture;

struct DownsampleParams
{
    float2 texelSize;      // 1 / the input's resolution
    float filterRadius;    // the upsample's; unused here
    float isFirstPass;     // > 0.5: the Karis average
};
[[vk::push_constant]] ConstantBuffer<DownsampleParams> params;

float karisWeight(float3 c)
{
    return 1.0 / (max(max(c.r, c.g), c.b) + 1.0);
}

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float2 texel = params.texelSize;

    // a - b - c
    // - d - e -
    // f - g - h
    // - i - j -
    // k - l - m
    const float3 a = inputTexture.Sample(uv + texel * float2(-2, -2)).rgb;
    const float3 b = inputTexture.Sample(uv + texel * float2( 0, -2)).rgb;
    const float3 c = inputTexture.Sample(uv + texel * float2( 2, -2)).rgb;

    const float3 d = inputTexture.Sample(uv + texel * float2(-1, -1)).rgb;
    const float3 e = inputTexture.Sample(uv + texel * float2( 1, -1)).rgb;

    const float3 f = inputTexture.Sample(uv + texel * float2(-2,  0)).rgb;
    const float3 g = inputTexture.Sample(uv).rgb;
    const float3 h = inputTexture.Sample(uv + texel * float2( 2,  0)).rgb;

    const float3 i = inputTexture.Sample(uv + texel * float2(-1,  1)).rgb;
    const float3 j = inputTexture.Sample(uv + texel * float2( 1,  1)).rgb;

    const float3 k = inputTexture.Sample(uv + texel * float2(-2,  2)).rgb;
    const float3 l = inputTexture.Sample(uv + texel * float2( 0,  2)).rgb;
    const float3 m = inputTexture.Sample(uv + texel * float2( 2,  2)).rgb;

    float3 bloom;
    if (params.isFirstPass > 0.5)
    {
        const float3 g1 = (a + b + f + g) * 0.25;
        const float3 g2 = (b + c + g + h) * 0.25;
        const float3 g3 = (f + g + k + l) * 0.25;
        const float3 g4 = (g + h + l + m) * 0.25;
        const float3 g5 = (d + e + i + j) * 0.25;

        const float w1 = karisWeight(g1);
        const float w2 = karisWeight(g2);
        const float w3 = karisWeight(g3);
        const float w4 = karisWeight(g4);
        const float w5 = karisWeight(g5);

        const float kw1 = 0.125 * w1, kw2 = 0.125 * w2;
        const float kw3 = 0.125 * w3, kw4 = 0.125 * w4;
        const float kw5 = 0.5 * w5;

        bloom = (g1 * kw1 + g2 * kw2 + g3 * kw3 + g4 * kw4 + g5 * kw5) / (kw1 + kw2 + kw3 + kw4 + kw5);
    }
    else
    {
        const float3 group1 = (a + b + f + g) * 0.03125;   // 1/32
        const float3 group2 = (b + c + g + h) * 0.03125;
        const float3 group3 = (f + g + k + l) * 0.03125;
        const float3 group4 = (g + h + l + m) * 0.03125;
        const float3 group5 = (d + e + i + j) * 0.125;     // 1/8

        bloom = group1 + group2 + group3 + group4 + group5;
    }
    return float4(bloom, 1.0);
}
