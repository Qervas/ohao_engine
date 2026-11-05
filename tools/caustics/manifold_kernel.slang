// shaders/rt/manifold.slang on the CPU: slangc compiles this kernel to C++ (-target cpp) and
// tests/renderer/manifold_tests calls it, so the tests run the path tracer's own solver. Outside shaders/: never
// compiled to SPIR-V.
//
// One item is 20 float4s of `io`, rows 0-12 in, 13-19 out. Row 0 (op, tol, slope2, thetaSigma); rows 1-6 the
// triangle (p0, e1, e2, n0, a1, a2); row 7 p; row 8 L; row 9 (uv or sigma, alphas); row 10 u or the box's min;
// row 11 the box's max; row 12 (axis, half angle). Ops:
//   0 map      at uv 9.xy           -> 13 (sigma, ok), 14 (du, dv), 15 y, 16 n
//   1 solve    target 9.xy, tol     -> 13 (uv, converged)
//   2 draw     alphas 9.zw, u 10, the core (0.z, 11.x)    -> 13 (sigma, its density)
//   3 density  alphas 9.zw, sigma 9.xy, the core (0.z, 11.x) -> 13.x
//   4 cone     box 10-11, cone 12, p, L, thetaSigma -> 13.x (1: may hold a root)
//   5 tri      the triangle, p, L, sigma 9.xy -> 13.x (manifoldTriangleTest)
//   6 disc     box 10-11, cone 12, c (the triangle's), sigma 9.xy, p, L -> 13.x (manifoldDiscTest)
// The triangle's c rides in rows 1-3's w.
import rt.manifold;

RWStructuredBuffer<float4> io;

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint b = id.x * 20u;
    const float4 h = io[b];
    const uint op = uint(h.x + 0.5);
    ManifoldTri tri;
    tri.p0 = io[b + 1u].xyz;
    tri.e1 = io[b + 2u].xyz;
    tri.e2 = io[b + 3u].xyz;
    tri.n0 = io[b + 4u].xyz;
    tri.a1 = io[b + 5u].xyz;
    tri.a2 = io[b + 6u].xyz;
    tri.c = float3(io[b + 1u].w, io[b + 2u].w, io[b + 3u].w);
    const float3 p = io[b + 7u].xyz, L = io[b + 8u].xyz;
    const float4 r9 = io[b + 9u];
    float4 o13 = float4(0.0), o14 = float4(0.0), o15 = float4(0.0), o16 = float4(0.0);
    if (op == 0u)
    {
        float2 sigma, du, dv;
        float3 y, n;
        const bool ok = manifoldMap(tri, p, L, r9.xy, sigma, du, dv, y, n);
        o13 = float4(sigma, ok ? 1.0 : 0.0, 0.0);
        o14 = float4(du, dv);
        o15 = float4(y, 0.0);
        o16 = float4(n, 0.0);
    }
    else if (op == 1u)
    {
        float2 uv;
        const bool ok = manifoldSolve(tri, p, L, r9.xy, h.y, uv);
        o13 = float4(uv, ok ? 1.0 : 0.0, 0.0);
    }
    else if (op == 2u)
    {
        const float2 core = float2(h.z, io[b + 11u].x);
        const float2 s = manifoldDrawSlope(r9.zw, core, io[b + 10u].xyz);
        o13 = float4(s, manifoldSlopeDensity(r9.zw, core, s), 0.0);
    }
    else if (op == 3u)
    {
        o13.x = manifoldSlopeDensity(r9.zw, float2(h.z, io[b + 11u].x), r9.xy);
    }
    else if (op == 4u)
    {
        const float4 c = io[b + 12u];
        o13.x = manifoldConeTest(io[b + 10u].xyz, io[b + 11u].xyz, c.xyz, c.w, p, L, h.w) ? 1.0 : 0.0;
    }
    else if (op == 5u)
    {
        o13.x = manifoldTriangleTest(tri, p, L, r9.xy) ? 1.0 : 0.0;
    }
    else if (op == 6u)
    {
        const float4 c = io[b + 12u];
        o13.x = manifoldDiscTest(io[b + 10u].xyz, io[b + 11u].xyz, c.xyz, c.w, tri.c, r9.xy, p, L) ? 1.0 : 0.0;
    }
    io[b + 13u] = o13;
    io[b + 14u] = o14;
    io[b + 15u] = o15;
    io[b + 16u] = o16;
}
