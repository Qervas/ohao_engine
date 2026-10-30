// The deferred frame's display pass: bloom added to the lit HDR image, then the display transform
// (display/display.slang): exposure (fixed, or a window's metered EV100), the curve (AgX or PBR
// Neutral), sRGB.
import display.display;

[[vk::binding(0, 0)]] Sampler2D hdrInput;
[[vk::binding(1, 0)]] Sampler2D bloomInput;
[[vk::binding(2, 0)]] StructuredBuffer<float4> exposureState;   // auto exposure (postprocess/exposure.slang): x EV100

struct DisplayParams
{
    float exposure;        // linear: 1 / (1.2 x 2^EV100)
    float bloomStrength;   // 0: no bloom
    uint curve;            // kDisplayAgX | kDisplayNeutral
    uint autoExposure;     // 1: the metered EV100 in place of `exposure`
};
[[vk::push_constant]] ConstantBuffer<DisplayParams> params;

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    const float3 hdr = hdrInput.Sample(uv).rgb + bloomInput.Sample(uv).rgb * params.bloomStrength;
    const float exposure = params.autoExposure != 0u ? displayExposure(exposureState[0].x) : params.exposure;
    return float4(displayPixel(hdr, exposure, params.curve), 1.0);
}
