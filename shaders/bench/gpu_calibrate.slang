// render_bench's GPU calibration kernel (sub-project 2b spec, "Profiling first"): a fixed
// integer workload over a 1 MiB buffer, timed by timestamps before and after a bench's
// frames. A run on a GPU that something else is using takes longer than the reference.
// No memory traffic beyond one load and one store per invocation: it measures the shader
// cores' clock and occupancy, not bandwidth. The first Slang shader (2b step 6).
import shared.layout;

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> data;
[[vk::push_constant]] ConstantBuffer<CalibratePush> pc;

[shader("compute")]
[numthreads(kCalibrateGroupSize, 1, 1)]
void calibrate(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    uint x = data[i] ^ (i * 2654435761u);
    for (uint k = 0u; k < pc.iterations; ++k)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x = x * 1664525u + 1013904223u;
    }
    data[i] = x;
}
