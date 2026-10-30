// shaders/display/display.slang on the CPU: slangc compiles this kernel to C++ (-target cpp), and
// tests/renderer/display_test calls it, so the test runs the GPU's own display transform. Outside
// shaders/: never compiled to SPIR-V.
//
// One item is 2 float4s of `io`: in 0 (radiance rgb, exposure), out 1 (encoded rgb, curve as bits in).
import display.display;

RWStructuredBuffer<float4> io;

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const float4 in = io[id.x * 2u];
    const uint curve = asuint(io[id.x * 2u + 1u].w);
    io[id.x * 2u + 1u] = float4(displayPixel(in.rgb, in.w, curve), 0.0);
}
