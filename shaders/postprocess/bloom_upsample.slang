// Bloom's upsample: a 9-tap tent (1 2 1 / 2 4 2 / 1 2 1) over the lower mip, scaled by blendFactor;
// the pipeline's blend (ONE, ONE) adds it to the level it draws into.

[[vk::binding(0, 0)]] Sampler2D inputTexture;   // the lower mip

struct UpsampleParams
{
    float2 texelSize;      // 1 / the lower mip's resolution
    float filterRadius;
    float blendFactor;
};
[[vk::push_constant]] ConstantBuffer<UpsampleParams> params;

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float2 offset = params.texelSize * params.filterRadius;

    const float3 a = inputTexture.Sample(uv + float2(-offset.x, -offset.y)).rgb;
    const float3 b = inputTexture.Sample(uv + float2(      0.0, -offset.y)).rgb;
    const float3 c = inputTexture.Sample(uv + float2( offset.x, -offset.y)).rgb;

    const float3 d = inputTexture.Sample(uv + float2(-offset.x,       0.0)).rgb;
    const float3 e = inputTexture.Sample(uv).rgb;
    const float3 f = inputTexture.Sample(uv + float2( offset.x,       0.0)).rgb;

    const float3 g = inputTexture.Sample(uv + float2(-offset.x,  offset.y)).rgb;
    const float3 h = inputTexture.Sample(uv + float2(      0.0,  offset.y)).rgb;
    const float3 i = inputTexture.Sample(uv + float2( offset.x,  offset.y)).rgb;

    float3 upsampled = e * 4.0;
    upsampled += (b + d + f + h) * 2.0;
    upsampled += (a + c + g + i);
    upsampled *= (1.0 / 16.0);
    return float4(upsampled * params.blendFactor, 1.0);
}
