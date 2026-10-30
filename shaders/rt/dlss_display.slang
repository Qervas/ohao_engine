// DLSS Ray Reconstruction's frame: DLSS writes a denoised HDR-linear image (COLOR_OUT, RGBA16F) at
// the output's size; this pass writes it to the frame's output through the display transform
// (display/display.slang), as every other path does.
import display.display;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> hdrIn;   // DLSS COLOR_OUT
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> ldrOut;    // the frame's output

struct DisplayParams
{
    float exposure;
    uint curve;
};
[[vk::push_constant]] ConstantBuffer<DisplayParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    hdrIn.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    ldrOut[id.xy] = float4(displayPixel(max(hdrIn[id.xy].rgb, 0.0), params.exposure, params.curve), 1.0);
}
