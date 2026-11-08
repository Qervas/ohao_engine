// shaders/rt/manifold.slang on the CPU: slangc compiles this kernel to C++ (-target cpp) and
// tests/renderer/manifold_tests calls it, so the tests run the path tracer's own solver. Outside shaders/: never
// compiled to SPIR-V.
//
// One item is 32 float4s of `io`, rows 0-12 in, 13-19 out (op 9: rows 0-12 and 17-22 in, 23-27 out). Row 0 (op,
// tol, slope2, thetaSigma); rows 1-6 the triangle (p0, e1, e2, n0, a1, a2); row 7 p; row 8 L; row 9 (uv or sigma, alphas); row 10 u or the box's min;
// row 11 the box's max; row 12 (axis, half angle). Ops:
//   0 map      at uv 9.xy           -> 13 (sigma, ok), 14 (du, dv), 15 y, 16 n
//   1 solve    target 9.xy, tol     -> 13 (uv, converged)
//   2 draw     alphas 9.zw, u 10, the core (0.z, 11.x)    -> 13 (sigma, its density)
//   3 density  alphas 9.zw, sigma 9.xy, the core (0.z, 11.x) -> 13.x
//   4 cone     box 10-11, cone 12, p, L, thetaSigma -> 13.x (1: may hold a root)
//   5 tri      the triangle, p, L, sigma 9.xy -> 13.x (manifoldTriangleTest)
//   6 disc     box 10-11, cone 12, c (the triangle's), sigma 9.xy, p, L -> 13.x (manifoldDiscTest)
//   7 certify  target 9.xy -> 13 (roots, undecided, boxes), 14 (uv0, uv1), 15 (uv2, uv3) (manifoldCertify)
//   8 enclose  the box (u0, v0) 10.xy to (u1, v1) 11.xy -> 13 (sx lo hi, sy lo hi), 14 (dsx/du, dsx/dv lo hi),
//              15 (dsy/du, dsy/dv lo hi), 16.x defined (1, 0, -1) (manifoldSigmaT over intervals)
//   9 pair     the entry triangle rows 1-6, the exit triangle rows 17-22 (its c in 17-19's w), X row 7, (L, eta)
//              row 8, the slopes (s1, s2) row 9 -> 23 (roots, undecided, boxes), 24-27 each root (u1, v1, u2, v2)
//              (manifoldCertifyPair)
//  12 walk      X row 7, (L, eta) row 8, the slopes row 9, the seed's direction row 10, the scene in `mesh` (6 rows a triangle, its c in rows
//              0-2's w) -> 23 (found, steps, roots, undecided), 24 (u1, v1, u2, v2), 25 (entry, exit, slopeDet)
//              (manifoldWalkPair, the final pair certified)
//  13 walk      as 12, uncertified (the GPU's walk: a path is where it converges)
//  14 cell      step 2c: the triangle rows 1-6, its tangents rows 17-19 (17.w the bitangent's sign), (uv0, uvA) row 20,
//              (uvB, the cell's origin) row 21, a texel's size 22.xy, the cell's texels' normals c00, c10, c01, c11 rows
//              23-26, p, L, the target 9.xy -> 13 (roots, undecided, boxes, defined), 14 (st0, st1), 15 (st2, st3)
//              (manifoldCertifyMap on a CellSlopeMap: every root in the cell, as (s, t))
//  15 cell map  the same cell at (s, t) 9.zw -> 13 (sigma, defined), 14 (dsx/ds, dsx/dt, dsy/ds, dsy/dt), 15 (u, v)
//  16 cell poly step 2c's polynomials (rt/manifold_poly.slang, MikkTSpace's normal) of the same cell for the slope 9.xy:
//              at (s, t) 9.zw -> 13 (F1, F2 from the grids, their rounding bounds mu1, mu2), 14 (r . a, r . b from the
//              corner values, defined); the grids' coefficients into `mesh` rows 0-8 (F1) and 9-17 (F2)
//  17 cell roots the same cell's paths for the slope 9.xy, at most 0.y boxes (0: 4,096) -> 13 (roots, undecided, boxes,
//              defined), 14 (st0, st1), 15 (st2, st3), 16 (st4, overflow) (cellPolySolve)
// The triangle's c rides in rows 1-3's w.
import rt.manifold;
import rt.manifold_cert;
import rt.manifold_poly;

RWStructuredBuffer<float4> io;
RWStructuredBuffer<float4> mesh;   // op 12's scene: 6 rows a triangle (p0, e1, e2, n0, a1, a2), its c in rows 0-2's w

ManifoldTri meshTriangle(uint i)
{
    const uint m = i * 6u;
    ManifoldTri t;
    t.p0 = mesh[m].xyz;
    t.e1 = mesh[m + 1u].xyz;
    t.e2 = mesh[m + 2u].xyz;
    t.n0 = mesh[m + 3u].xyz;
    t.a1 = mesh[m + 4u].xyz;
    t.a2 = mesh[m + 5u].xyz;
    t.c = float3(mesh[m].w, mesh[m + 1u].w, mesh[m + 2u].w);
    return t;
}

// Every triangle of `mesh`, tested in turn (Moller-Trumbore): the CPU twin's scene for the walk.
struct BufferScene : IManifoldScene
{
    uint count;
    bool trace(float3 o, float3 d, float tMax, out uint tri, out float2 uv, out float t)
    {
        tri = 0u;
        uv = float2(0.0);
        t = tMax;
        bool hit = false;
        for (uint i = 0u; i < count; ++i)
        {
            const ManifoldTri m = meshTriangle(i);
            const float3 pv = cross(d, m.e2);
            const float det = dot(m.e1, pv);
            if (abs(det) < 1e-12) continue;
            const float3 tv = o - m.p0;
            const float a = dot(tv, pv) / det;
            if (a < 0.0 || a > 1.0) continue;
            const float3 qv = cross(tv, m.e1);
            const float b = dot(d, qv) / det;
            if (b < 0.0 || a + b > 1.0) continue;
            const float th = dot(m.e2, qv) / det;
            if (th > 1e-6 && th < t)
            {
                t = th;
                tri = i;
                uv = float2(a, b);
                hit = true;
            }
        }
        return hit;
    }
    ManifoldTri triangle(uint i) { return meshTriangle(i); }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint b = id.x * 32u;
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
    else if (op == 7u)
    {
        const float4 bl = io[b + 10u], bh = io[b + 11u];
        const CertResult r = bh.x > 0.0 ? manifoldCertifyBox(tri, p, L, r9.xy, float4(bl.xy, bh.xy))
                                        : manifoldCertify(tri, p, L, r9.xy);
        o13 = float4(float(r.roots), float(r.undecided), float(r.boxes), 0.0);
        o14 = float4(r.uv[0], r.uv[1]);
        o15 = float4(r.uv[2], r.uv[3]);
    }
    else if (op == 8u)
    {
        const float4 lo = io[b + 10u], hi = io[b + 11u];
        MDualI2 sx, sy;
        const MDualI2 iu = MDualI2.variable(MIval.make(lo.x, hi.x), 0), iv = MDualI2.variable(MIval.make(lo.y, hi.y), 1);
        // 0.y = 1: the constraint F = (A - target.x C, B - target.y C) with target 9.xy instead of sigma.
        const int def = h.y > 0.5 ? manifoldConstraintT<MDualI2>(tri, p, L, r9.xy, iu, iv, sx, sy)
                                  : manifoldSigmaT<MDualI2>(tri, p, L, iu, iv, sx, sy);
        o13 = float4(sx.v.lo, sx.v.hi, sy.v.lo, sy.v.hi);
        o14 = float4(sx.d[0].lo, sx.d[0].hi, sx.d[1].lo, sx.d[1].hi);
        o15 = float4(sy.d[0].lo, sy.d[0].hi, sy.d[1].lo, sy.d[1].hi);
        o16 = float4(float(def), 0.0, 0.0, 0.0);
    }
    else if (op == 9u)
    {
        ManifoldTri t2;
        t2.p0 = io[b + 17u].xyz;
        t2.e1 = io[b + 18u].xyz;
        t2.e2 = io[b + 19u].xyz;
        t2.n0 = io[b + 20u].xyz;
        t2.a1 = io[b + 21u].xyz;
        t2.a2 = io[b + 22u].xyz;
        t2.c = float3(io[b + 17u].w, io[b + 18u].w, io[b + 19u].w);
        const float4 r8 = io[b + 8u];
        const ManifoldPairResult r = manifoldCertifyPair(tri, t2, p, r8.xyz, r8.w, r9);
        io[b + 23u] = float4(float(r.roots), float(r.undecided), float(r.boxes), 0.0);
        [ForceUnroll] for (uint k = 0u; k < 4u; ++k) io[b + 24u + k] = r.uv[k];
    }
    else if (op == 12u || op == 13u)
    {
        BufferScene scene;
        scene.count = uint(h.y + 0.5);
        const float4 r8 = io[b + 8u];
        ManifoldWalkResult r;
        if (op == 12u) r = manifoldWalkPair<BufferScene, true>(scene, p, r8.xyz, r8.w, r9, io[b + 10u].xyz);
        else r = manifoldWalkPair<BufferScene, false>(scene, p, r8.xyz, r8.w, r9, io[b + 10u].xyz);
        io[b + 23u] = float4(r.found ? 1.0 : 0.0, float(r.steps), float(r.roots), float(r.undecided));
        io[b + 24u] = r.x;
        io[b + 25u] = float4(float(r.entry), float(r.exit), r.slopeDet, 0.0);
    }
    else if (op == 14u || op == 15u)
    {
        ManifoldMappedTri mt;
        mt.tri = tri;
        mt.t0 = io[b + 17u].xyz;
        mt.sign = io[b + 17u].w;
        mt.ta1 = io[b + 18u].xyz;
        mt.ta2 = io[b + 19u].xyz;
        const float4 r20 = io[b + 20u], r21 = io[b + 21u];
        mt.uv0 = r20.xy;
        mt.uvA = r20.zw;
        mt.uvB = r21.xy;
        CellSlopeMap cm;
        const bool ok = cellSlopeMap(mt, p, L, io[b + 23u].xyz, io[b + 24u].xyz, io[b + 25u].xyz, io[b + 26u].xyz,
                                     r21.zw, io[b + 22u].xy, cm);
        if (op == 14u)
        {
            CertResult r;
            r.roots = 0u;
            r.undecided = 0u;
            r.boxes = 0u;
            [ForceUnroll] for (uint k = 0u; k < 4u; ++k) r.uv[k] = float2(0.0);
            if (ok) r = manifoldCertifyMap<CellSlopeMap>(cm, r9.xy, float4(0.0, 0.0, 1.0, 1.0));
            o13 = float4(float(r.roots), float(r.undecided), float(r.boxes), ok ? 1.0 : 0.0);
            o14 = float4(r.uv[0], r.uv[1]);
            o15 = float4(r.uv[2], r.uv[3]);
        }
        else
        {
            MDualF2 A = MDualF2.cst(0.0), B = MDualF2.cst(0.0), C = MDualF2.cst(1.0);
            const int def = ok ? cm.frame<MDualF2>(MDualF2.variable(MFloat.cst(r9.z), 0),
                                                   MDualF2.variable(MFloat.cst(r9.w), 1), A, B, C, 0)
                               : 0;
            const bool good = def == 1 && C.v.v > 0.0;
            const MDualF2 sx = A.div(C), sy = B.div(C);
            o13 = float4(good ? sx.v.v : 0.0, good ? sy.v.v : 0.0, good ? 1.0 : 0.0, 0.0);
            o14 = good ? float4(sx.d[0].v, sx.d[1].v, sy.d[0].v, sy.d[1].v) : float4(0.0);
            o15 = float4(cm.uvAt(r9.zw), 0.0, 0.0);
        }
    }
    else if (op == 16u || op == 17u)
    {
        ManifoldMappedTri mt;
        mt.tri = tri;
        mt.t0 = io[b + 17u].xyz;
        mt.sign = io[b + 17u].w;
        mt.ta1 = io[b + 18u].xyz;
        mt.ta2 = io[b + 19u].xyz;
        const float4 r20 = io[b + 20u], r21 = io[b + 21u];
        mt.uv0 = r20.xy;
        mt.uvA = r20.zw;
        mt.uvB = r21.xy;
        CellPoly cp;
        const bool ok = cellPolyOf(mt, io[b + 23u].xyz, io[b + 24u].xyz, io[b + 25u].xyz, io[b + 26u].xyz, r21.zw,
                                   io[b + 22u].xy, p, L, r9.xy, cp);
        if (op == 16u)
        {
            if (ok) cellPolyRounding(cp);
            float3 hv, y;
            const float3 r = polyReflectAt(cp, r9.zw, hv, y);
            o13 = float4(bEval(cp.F1, r9.z, r9.w), bEval(cp.F2, r9.z, r9.w), cp.mu1, cp.mu2);
            o14 = float4(dot(r, cp.a), dot(r, cp.b), ok ? 1.0 : 0.0, 0.0);
            for (uint k = 0u; k < 9u; ++k)
            {
                mesh[k] = float4(cp.F1.c[4u * k], cp.F1.c[4u * k + 1u], cp.F1.c[4u * k + 2u], cp.F1.c[4u * k + 3u]);
                mesh[9u + k] = float4(cp.F2.c[4u * k], cp.F2.c[4u * k + 1u], cp.F2.c[4u * k + 2u], cp.F2.c[4u * k + 3u]);
            }
        }
        else
        {
            const uint budget = h.y > 0.5 ? uint(h.y + 0.5) : 4096u;
            PolyCellResult r;
            r.roots = 0u;
            r.undecided = 0u;
            r.boxes = 0u;
            r.overflow = 0u;
            for (uint k = 0u; k < kPolyMaxRoots; ++k) r.st[k] = float2(0.0);
            if (ok) r = cellPolySolve(cp, budget);
            o13 = float4(float(r.roots), float(r.undecided), float(r.boxes), ok ? 1.0 : 0.0);
            o14 = float4(r.st[0], r.st[1]);
            o15 = float4(r.st[2], r.st[3]);
            o16 = float4(r.st[4], float(r.overflow), 0.0);
        }
    }
    io[b + 13u] = o13;
    io[b + 14u] = o14;
    io[b + 15u] = o15;
    io[b + 16u] = o16;
}
