// Bloom's input (frontier F2 T4): the frame's radiance as it is, into the chain's level 0. No
// threshold and no clamp: bloom is a convolution of the whole frame's energy (Jimenez, SIGGRAPH
// 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare"), mixed into the frame
// at a small strength by the display pass, so a light glares by its radiance and nothing is added.
// The first downsample's Karis average keeps a single bright texel from flickering.

[[vk::binding(0, 0)]] Sampler2D hdrInput;

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float3 c = hdrInput.Sample(uv).rgb;
    return float4(all(isfinite(c)) ? max(c, 0.0) : float3(0.0), 1.0);
}
