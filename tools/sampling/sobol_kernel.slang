// shaders/sampling/sobol.slang on the CPU: slangc compiles this kernel to C++ (-target cpp), and
// tests/renderer/sobol_test calls it, so the test runs the path tracer's own sampler. Outside
// shaders/: never compiled to SPIR-V.
//
// One item is 2 uint4s of `io`: in 0 (index, seed, group, -), out 1 the group's 4-D point as
// float bits (Sampler(index, seed) moved to `group`, four next() draws).
import sampling.sobol;

RWStructuredBuffer<uint4> io;

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint4 in = io[id.x * 2u];
    Sampler s = Sampler(in.x, in.y);
    s.group(in.z);
    const float4 p = float4(s.next(), s.next(), s.next(), s.next());
    io[id.x * 2u + 1u] = asuint(p);
}
