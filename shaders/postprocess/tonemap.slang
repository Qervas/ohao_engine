// The deferred frame's display pass: bloom added to the lit HDR image, then the display transform
// (display/display.slang): exposure, the curve (AgX or PBR Neutral), sRGB.
import display.display;

[[vk::binding(0, 0)]] Sampler2D hdrInput;
[[vk::binding(1, 0)]] Sampler2D bloomInput;

struct DisplayParams
{
    float exposure;        // linear: 1 / (1.2 x 2^EV100)
    float bloomStrength;   // 0: no bloom
    uint curve;            // kDisplayAgX | kDisplayNeutral
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<DisplayParams> params;

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float3 hdr = hdrInput.Sample(uv).rgb + bloomInput.Sample(uv).rgb * params.bloomStrength;
    return float4(displayPixel(hdr, params.exposure, params.curve), 1.0);
}
