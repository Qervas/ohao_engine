// shaders/sampling/vmf.slang on the CPU: slangc compiles this kernel to C++ (-target cpp), and
// tests/renderer/vmf_test calls it, so the test runs the path tracer's own lobes. Outside
// shaders/: never compiled to SPIR-V.
//
// One item is 6 float4s of `io`, rows 0-3 in, 4-5 out; row 0.x is the operation:
//   0 pdf      row 1.xyz dir, row 2.xyz mu, row 2.w kappa          -> row 4.x
//   1 sample   row 1.xy u, row 2.xyz mu, row 2.w kappa             -> row 4.xyz
//   2 kappa    row 1.x mean cosine                                 -> row 4.x
//   3 meanCos  row 1.x kappa                                       -> row 4.x
//   4 update   row 1 (w, y), row 2 (r, nt bits, check bits, frame), row 3 (x, f), row 0.yzw target
//                                                                  -> row 4 (w, y), row 5 (r, nt bits)
//   5 lobe     row 1 (w, y), row 2 (r, nt bits), row 3.xyz x       -> row 4 (mu, kappa), row 5.x valid
import sampling.vmf;

RWStructuredBuffer<float4> io;

GuideState stateOf(float4 a, float4 b)
{
    GuideState s = guideEmpty();
    s.w = a.x;
    s.y = a.yzw;
    s.r = b.x;
    s.nt = asuint(b.y);
    s.check = asuint(b.z);
    return s;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint base = id.x * 6u;
    const float4 r0 = io[base], r1 = io[base + 1u], r2 = io[base + 2u], r3 = io[base + 3u];
    const uint op = uint(r0.x);
    float4 o4 = float4(0.0), o5 = float4(0.0);
    if (op == 0u) o4.x = vmfPdf(r1.xyz, r2.xyz, r2.w);
    else if (op == 1u) o4.xyz = vmfSample(r2.xyz, r2.w, r1.xy);
    else if (op == 2u) o4.x = vmfKappa(r1.x);
    else if (op == 3u) o4.x = vmfMeanCosine(r1.x);
    else if (op == 4u)
    {
        const GuideState s = guideUpdate(stateOf(r1, r2), r3.xyz, r0.yzw, r3.w, uint(r2.w));
        o4 = float4(s.w, s.y);
        o5 = float4(s.r, asfloat(s.nt), asfloat(s.check), 0.0);
    }
    else if (op == 5u)
    {
        float3 mu;
        float kappa;
        const bool ok = guideLobe(stateOf(r1, r2), r3.xyz, mu, kappa);
        o4 = float4(mu, kappa);
        o5.x = ok ? 1.0 : 0.0;
    }
    io[base + 4u] = o4;
    io[base + 5u] = o5;
}
