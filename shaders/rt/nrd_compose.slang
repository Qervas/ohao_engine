// The NRD frame: NRD's denoised diffuse remodulated by the first hit's diffuse albedo (the tracer
// divided it out), its denoised specular as it is (the tracer did not), and the camera ray's
// emission, which never passes through NRD:
//   composed = denoisedDiff * diffAlbedo + denoisedSpec + emission
// written as HDR and as the frame's output through the display transform (display/display.slang),
// so a window presents the denoised frame and a capture reads it. REBLUR's output is YCoCg.
import display.display;

[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> inDiffRad;     // NRD's denoised diffuse
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> inSpecRad;     // NRD's denoised specular
[[vk::binding(2, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> inDiffAlbedo;    // the diffuse albedo (PT 24)
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> inSpecColor;     // unused: DLSS's specular guide (PT 25)
[[vk::binding(4, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outComposed;   // HDR
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> inEmission;    // the camera ray's emission (PT 13)
[[vk::binding(6, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outLdr;          // the frame's output (PT 2)

struct DisplayParams
{
    float exposure;
    uint curve;
};
[[vk::push_constant]] ConstantBuffer<DisplayParams> params;

// REBLUR's back end: YCoCg to linear (NRD.hlsli, REBLUR_BackEnd_UnpackRadianceAndNormHitDist).
float3 yCoCgToLinear(float3 c)
{
    const float t = c.x - c.z;
    return max(float3(t + c.y, c.x + c.z, t - c.y), 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    outComposed.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const uint2 p = id.xy;
    const float3 composed = yCoCgToLinear(inDiffRad[p].rgb) * inDiffAlbedo[p].rgb + yCoCgToLinear(inSpecRad[p].rgb) +
                            inEmission[p].rgb;
    outComposed[p] = float4(composed, 1.0);
    outLdr[p] = float4(displayPixel(composed, params.exposure, params.curve), 1.0);
}
