// Step 2c's cell solver (docs/specs/2026-11-04-caustics-step2-design.md, "Step 2c", T2; rulings M11b, M13; the plan
// docs/plans/2026-11-08-caustics-step2c.md, tasks 1 and 2): on one cell of a normal map (the square between four texel
// centres, (s, t) in [0, 1]^2) of one triangle, the reflection constraint as two polynomials in tensor Bernstein form,
// and every root of them by subdivision and Krawczyk's test. A module (no entry point).
//
// The polynomials. MikkTSpace's mapped normal (ruling M13) from the interpolated vectors as they are, vB the
// interpolation of each vertex's w N x T, and the map's four texels blended bilinearly:
//   m = tn.x vT + tn.y vB + tn.z vN          bidegree (2, 2)
//   h = m - sigma.x vT - sigma.y vB           the drawn slope's microfacet normal (the vertex tangent frame), (2, 2)
//   d = y - p                                 y the surface point, p the receiver (X), (1, 1)
//   r = d (h . h) - 2 (d . h) h               |h|^2 times d reflected about h's plane: |h|^2 |d| L at a path, (5, 5)
//   F1 = r . a, F2 = r . b                    a, b perpendicular to L (toward the sun)
// A root of F is a path only where r . L > 0 (else d reflects to -L) and h . H > 0, H = normalize(p - y) + L (h's sign
// turned reflects the same, but is no microfacet the slope drew).
//
// Bernstein form: f(s, t) = sum c_ij B_i^ns(s) B_j^nt(t). Products, degree elevation and de Casteljau's subdivision
// are convex combinations, so the grids are built in fp32 from the cell's corner values with no cancellation of their
// own; a grid's hull encloses its polynomial over its box (no dependency problem: T1's interval enclosures overestimated
// F many times over). The resultant was refused (T2: its determinant underflows fp32 and a fifth of the roots are lost).
//
// What is solved exactly: the fp32 grids, the cell as the GPU holds it, as fp32 vertex positions are the mesh. Their
// distance from the double polynomials is bounded (mu: the same construction on the inputs' absolute values times a
// count of roundings) and measured (m11_poly_matches_map: at most 5.5 roundings); it moves a root by |Y| mu, as fp32
// positions do, and is not the solver's to remove: kept as Krawczyk's margin it blurred ill-conditioned roots wider than
// any box that could certify them (m12: 8% of cells undecided). The solver's own arithmetic (a box's extraction, the
// evaluations) is bounded and kept as a margin in every test, so it never excludes a root of the grids. A root reported
// is checked afterwards (in the cell, on the triangle, physical).

import rt.manifold;
import rt.manifold_cert;

static const float kPolyUlp = 5.9604645e-8;   // 2^-24: fp32's unit roundoff
// Roundings a coefficient's error is bounded by, in units of kPolyUlp times its terms' magnitude (the same construction
// on the inputs' absolute values). m11_poly_matches_map measures the fp32 grids against their double ones: at most 5.5
// (0.0107 of a bound of 512, 600 cells); 64 keeps ten times that. A bound too tight could exclude a root; too loose, it
// keeps boxes near a root from certifying (K's radius is at least |Y| times it).
static const float kPolyRoundings = 64.0;
// Roundings of a box's extraction from the cell's grid (two de Casteljau passes of 5 levels a direction), in units of
// kPolyUlp times the cell grid's largest coefficient.
static const float kPolyExtractRoundings = 24.0;
static const uint kPolyMaxRoots = 8u;
static const uint kPolyStack = 64u;
static const float kPolyMinWidth = 1.0e-6;   // a box narrower than this a side is undecided
static const float kPolyGrow = 0.1;          // each box is tested grown by this a side (epsilon inflation)

// Binomial coefficients to degree 5, row n at n (n + 1) / 2.
static const float kPolyBinom[21] = { 1.0,
                                      1.0, 1.0,
                                      1.0, 2.0, 1.0,
                                      1.0, 3.0, 3.0, 1.0,
                                      1.0, 4.0, 6.0, 4.0, 1.0,
                                      1.0, 5.0, 10.0, 10.0, 5.0, 1.0 };
float polyBinom(int n, int k) { return kPolyBinom[n * (n + 1) / 2 + k]; }

// A tensor Bernstein grid of degree (ns, nt) <= (5, 5): c[i * 6 + j], i along s, j along t. Every loop runs to the
// largest degree with the grid's own as a guard, so unrolled its indices are constants (an array indexed at run time
// lives in local memory).
struct BPoly
{
    float c[36];
    int ns, nt;
}

BPoly bZero(int ns, int nt)
{
    BPoly r;
    r.ns = ns;
    r.nt = nt;
    for (int k = 0; k < 36; ++k) r.c[k] = 0.0;
    return r;
}

// Degree (1, 1) from the values at (s, t) = (0, 0), (1, 0), (0, 1), (1, 1): a bilinear function's grid is its corners.
BPoly bBilinear(float f00, float f10, float f01, float f11)
{
    BPoly r = bZero(1, 1);
    r.c[0] = f00;
    r.c[1] = f01;
    r.c[6] = f10;
    r.c[7] = f11;
    return r;
}

// The product of a degree (A, A) grid and a (B, B) one, A + B <= 5: each coefficient a convex combination of products
// (the degrees compile-time constants, so the weights are too).
BPoly bMul<let A : int, let B : int>(BPoly a, BPoly b)
{
    BPoly r = bZero(A + B, A + B);
    [unroll] for (int i = 0; i <= A; ++i)
        [unroll] for (int k = 0; k <= B; ++k)
        {
            const float ws = polyBinom(A, i) * polyBinom(B, k) / polyBinom(A + B, i + k);
            [unroll] for (int j = 0; j <= A; ++j)
                [unroll] for (int l = 0; l <= B; ++l)
                {
                    const float w = ws * polyBinom(A, j) * polyBinom(B, l) / polyBinom(A + B, j + l);
                    r.c[(i + k) * 6 + j + l] += w * a.c[i * 6 + j] * b.c[k * 6 + l];
                }
        }
    return r;
}

// a + x b, both of one degree.
BPoly bAxpy(BPoly a, float x, BPoly b)
{
    BPoly r = a;
    for (int k = 0; k < 36; ++k) r.c[k] = a.c[k] + x * b.c[k];
    return r;
}

// A degree (1, 1) grid raised to (2, 2): the product with the constant 1.
BPoly bElevate(BPoly a)
{
    BPoly one = bZero(1, 1);
    for (int k = 0; k < 36; ++k) one.c[k] = 1.0;
    return bMul<1, 1>(a, one);
}

BPoly bAbs(BPoly a)
{
    BPoly r = a;
    for (int k = 0; k < 36; ++k) r.c[k] = abs(a.c[k]);
    return r;
}

float bMaxAbs(BPoly a)
{
    float m = 0.0;
    [unroll] for (int i = 0; i < 6; ++i)
        [unroll] for (int j = 0; j < 6; ++j)
            if (i <= a.ns && j <= a.nt) m = max(m, abs(a.c[i * 6 + j]));
    return m;
}

void bHull(BPoly a, out float lo, out float hi)
{
    lo = a.c[0];
    hi = a.c[0];
    [unroll] for (int i = 0; i < 6; ++i)
        [unroll] for (int j = 0; j < 6; ++j)
            if (i <= a.ns && j <= a.nt)
            {
                lo = min(lo, a.c[i * 6 + j]);
                hi = max(hi, a.c[i * 6 + j]);
            }
}

// The value at (s, t) of a degree (NS, NT) grid by de Casteljau (convex combinations inside [0, 1]^2).
float bEval<let NS : int, let NT : int>(BPoly a, float s, float t)
{
    float col[6];
    [unroll] for (int i = 0; i <= NS; ++i)
    {
        float w[6];
        [unroll] for (int j = 0; j <= NT; ++j) w[j] = a.c[i * 6 + j];
        [unroll] for (int q = 1; q <= NT; ++q)
            [unroll] for (int j = 0; j <= NT - q; ++j) w[j] = (1.0 - t) * w[j] + t * w[j + 1];
        col[i] = w[0];
    }
    [unroll] for (int q = 1; q <= NS; ++q)
        [unroll] for (int i = 0; i <= NS - q; ++i) col[i] = (1.0 - s) * col[i] + s * col[i + 1];
    return col[0];
}

// One row's Bernstein coefficients (degree N) on [0, 1] to those of the same polynomial on [a0, a1]: de Casteljau at a0
// keeping the right part, then at (a1 - a0) / (1 - a0) keeping the left.
void bRange1<let N : int>(inout float w[6], float a0, float a1)
{
    float t[6];
    float right[6];
    [unroll] for (int k = 0; k < 6; ++k)
    {
        t[k] = w[k];
        right[k] = w[k];
    }
    [unroll] for (int q = 1; q <= N; ++q)
    {
        [unroll] for (int j = 0; j <= N - q; ++j) t[j] = (1.0 - a0) * t[j] + a0 * t[j + 1];
        right[N - q] = t[N - q];
    }
    const float tau = (a1 - a0) / (1.0 - a0);
    float u[6];
    [unroll] for (int k = 0; k < 6; ++k) u[k] = right[k];
    w[0] = right[0];
    [unroll] for (int q = 1; q <= N; ++q)
    {
        [unroll] for (int j = 0; j <= N - q; ++j) u[j] = (1.0 - tau) * u[j] + tau * u[j + 1];
        w[q] = u[0];
    }
}

// The grid of the same polynomial, of degree (NS, NT), over the box [s0, s1] x [t0, t1] of [0, 1]^2 (s0 < 1, t0 < 1).
BPoly bBox<let NS : int, let NT : int>(BPoly a, float4 box)
{
    BPoly r = a;
    [unroll] for (int j = 0; j <= NT; ++j)
    {
        float w[6];
        [unroll] for (int i = 0; i < 6; ++i) w[i] = i <= NS ? r.c[i * 6 + j] : 0.0;
        bRange1<NS>(w, box.x, box.z);
        [unroll] for (int i = 0; i <= NS; ++i) r.c[i * 6 + j] = w[i];
    }
    [unroll] for (int i = 0; i <= NS; ++i)
    {
        float w[6];
        [unroll] for (int j = 0; j < 6; ++j) w[j] = j <= NT ? r.c[i * 6 + j] : 0.0;
        bRange1<NT>(w, box.y, box.w);
        [unroll] for (int j = 0; j <= NT; ++j) r.c[i * 6 + j] = w[j];
    }
    return r;
}

// The derivative along s (axis 0) or t (axis 1) in the grid's own parameters: degree one lower along it.
BPoly bDerivative(BPoly a, int axis)
{
    BPoly r = bZero(axis == 0 ? a.ns - 1 : a.ns, axis == 0 ? a.nt : a.nt - 1);
    [unroll] for (int i = 0; i < 6; ++i)
        [unroll] for (int j = 0; j < 6; ++j)
            if (i <= r.ns && j <= r.nt)
            {
                const float hi = axis == 0 ? a.c[min(i + 1, 5) * 6 + j] : a.c[i * 6 + min(j + 1, 5)];
                r.c[i * 6 + j] = float(axis == 0 ? a.ns : a.nt) * (hi - a.c[i * 6 + j]);
            }
    return r;
}

// ---- the cell -----------------------------------------------------------------------------------------------------

// What the solver and the checks at a root need of a cell: F1, F2 over [0, 1]^2 with their rounding bounds, and the
// corner values the polynomials were made from ((s, t) = (0, 0), (1, 0), (0, 1), (1, 1)), to check a root in fp32.
struct CellPoly
{
    BPoly F1, F2;
    BPoly D1s, D1t, D2s, D2t;  // their derivatives over the cell (cell units): a box's Jacobian is extracted from these
    float mu1, mu2;            // bounds on each grid's coefficients' rounding (absolute)
    float2 mF;                 // the largest coefficient of F1, F2 and of their derivatives' grids
    float4 mD;
    float3 y[4], vN[4], vT[4], vB[4], tn[4];
    float3 p, L, a, b;
    float2 sigma;
    float2 o, ds, dt;          // the triangle's (u, v) = o + ds s + dt t
}

float3 polyCorner(float3 c[4], float2 st)
{
    return lerp(lerp(c[0], c[1], st.x), lerp(c[2], c[3], st.x), st.y);
}

// A normal-mapped triangle: MikkTSpace's frame as the renderers build it (material/material_layers.slang,
// mikkTSpaceNormal): per vertex the unit world normal, the unit world tangent and w n x t, interpolated as they are.
// tri.n0, a1, a2 hold the normals (the vertices' unit normals, not 2a's unnormalised ones).
struct ManifoldMappedTri
{
    ManifoldTri tri;
    float3 t0, ta1, ta2;   // the tangents: t0, t1 - t0, t2 - t0
    float3 b0, ba1, ba2;   // the bitangents w n x t: b0, b1 - b0, b2 - b0
    float2 uv0, uvA, uvB;  // texture coordinates: uv0 + uvA u + uvB v
};

// MikkTSpace's frame at the triangle's (u, v).
void polyFrameAt(ManifoldMappedTri m, float2 uv, out float3 n, out float3 t, out float3 b)
{
    const ManifoldTri tri = m.tri;
    n = tri.n0 + tri.a1 * uv.x + tri.a2 * uv.y;
    t = m.t0 + m.ta1 * uv.x + m.ta2 * uv.y;
    b = m.b0 + m.ba1 * uv.x + m.ba2 * uv.y;
}

// r = d (h . h) - 2 (d . h) h at (s, t), from the corner values, in fp32: the checks at a root.
float3 polyReflectAt(CellPoly cp, float2 st, out float3 h, out float3 y)
{
    y = polyCorner(cp.y, st);
    const float3 n = polyCorner(cp.vN, st), t = polyCorner(cp.vT, st), b = polyCorner(cp.vB, st);
    const float3 tn = polyCorner(cp.tn, st);
    h = tn.x * t + tn.y * b + tn.z * n - cp.sigma.x * t - cp.sigma.y * b;
    const float3 d = y - cp.p;
    return d * dot(h, h) - 2.0 * dot(d, h) * h;
}

// The triangle's (u, v) at the cell's (s, t): o + ds s + dt t, the inverse of its texture coordinates' affine map at the
// cell (its (0, 0) at `origin`, a texel `size`). False when the texture coordinates are degenerate.
bool polyCellAffine(ManifoldMappedTri m, float2 origin, float2 size, out float2 o, out float2 ds, out float2 dt)
{
    o = float2(0.0);
    ds = float2(0.0);
    dt = float2(0.0);
    const float det = m.uvA.x * m.uvB.y - m.uvB.x * m.uvA.y;
    if (!(abs(det) > 1e-20)) return false;
    const float2x2 inv = float2x2(m.uvB.y, -m.uvB.x, -m.uvA.y, m.uvA.x) / det;
    o = mul(inv, origin - m.uv0);
    ds = float2(inv[0][0], inv[1][0]) * size.x;
    dt = float2(inv[0][1], inv[1][1]) * size.y;
    return true;
}

// The cell's polynomials for the slope sigma: the triangle m, its cell's texels' tangent-space normals at (s, t) = (0,
// 0), (1, 0), (0, 1), (1, 1), the texture coordinates of the cell's (0, 0) and a texel's size, the receiver p and the
// sun's direction L. False when the triangle's texture coordinates are degenerate.
bool cellPolyOf(ManifoldMappedTri m, float3 c00, float3 c10, float3 c01, float3 c11, float2 origin, float2 size,
                float3 p, float3 L, float2 sigma, out CellPoly cp)
{
    cp.F1 = bZero(5, 5);
    cp.F2 = cp.F1;
    cp.D1s = cp.F1;
    cp.D1t = cp.F1;
    cp.D2s = cp.F1;
    cp.D2t = cp.F1;
    cp.mu1 = 0.0;
    cp.mF = float2(0.0);
    cp.mD = float4(0.0);
    cp.mu2 = 0.0;
    cp.p = p;
    cp.L = L;
    cp.sigma = sigma;
    cp.o = float2(0.0);
    cp.ds = float2(0.0);
    cp.dt = float2(0.0);
    for (int k = 0; k < 4; ++k)
    {
        cp.y[k] = float3(0.0);
        cp.vN[k] = float3(0.0);
        cp.vT[k] = float3(0.0);
        cp.vB[k] = float3(0.0);
    }
    cp.tn[0] = c00;
    cp.tn[1] = c10;
    cp.tn[2] = c01;
    cp.tn[3] = c11;
    cp.a = normalize(abs(L.x) < 0.7 ? cross(L, float3(1.0, 0.0, 0.0)) : cross(L, float3(0.0, 1.0, 0.0)));
    cp.b = cross(L, cp.a);
    if (!polyCellAffine(m, origin, size, cp.o, cp.ds, cp.dt)) return false;
    for (int k = 0; k < 4; ++k)
    {
        const float2 st = float2(float(k & 1), float(k >> 1));
        const float2 uv = cp.o + cp.ds * st.x + cp.dt * st.y;
        cp.y[k] = m.tri.p0 + m.tri.e1 * uv.x + m.tri.e2 * uv.y;
        polyFrameAt(m, uv, cp.vN[k], cp.vT[k], cp.vB[k]);
    }
    BPoly F1, F2;
    cellPolyGrids<false>(cp, F1, F2);
    cp.F1 = F1;
    cp.F2 = F2;
    cp.D1s = bDerivative(cp.F1, 0);
    cp.D1t = bDerivative(cp.F1, 1);
    cp.D2s = bDerivative(cp.F2, 0);
    cp.D2t = bDerivative(cp.F2, 1);
    cp.mF = float2(bMaxAbs(cp.F1), bMaxAbs(cp.F2));
    cp.mD = float4(bMaxAbs(cp.D1s), bMaxAbs(cp.D1t), bMaxAbs(cp.D2s), bMaxAbs(cp.D2t));
    return true;
}

// F1 = (d . a)(h . h) - 2 (d . h)(h . a) and F2 (with b) over the cell, from the corner values, at one call site (each is
// compiled whole); MAGNITUDE: the same construction on every input's absolute value, each coefficient's terms'
// magnitudes, the scale of its rounding (the CPU twin's alone: a generic, so the GPU never compiles it).
void cellPolyGrids<let MAGNITUDE : bool>(CellPoly cp, out BPoly F1, out BPoly F2)
{
    BPoly hh = bZero(4, 4), dh = bZero(3, 3), ha = bZero(2, 2), hb = bZero(2, 2), da = bZero(1, 1), db = bZero(1, 1);
    for (int c = 0; c < 3; ++c)
    {
        BPoly T = bBilinear(cp.vT[0][c], cp.vT[1][c], cp.vT[2][c], cp.vT[3][c]);
        BPoly B = bBilinear(cp.vB[0][c], cp.vB[1][c], cp.vB[2][c], cp.vB[3][c]);
        BPoly N = bBilinear(cp.vN[0][c], cp.vN[1][c], cp.vN[2][c], cp.vN[3][c]);
        BPoly tx = bBilinear(cp.tn[0].x, cp.tn[1].x, cp.tn[2].x, cp.tn[3].x);
        BPoly ty = bBilinear(cp.tn[0].y, cp.tn[1].y, cp.tn[2].y, cp.tn[3].y);
        BPoly tz = bBilinear(cp.tn[0].z, cp.tn[1].z, cp.tn[2].z, cp.tn[3].z);
        BPoly d = bBilinear(cp.y[0][c] - cp.p[c], cp.y[1][c] - cp.p[c], cp.y[2][c] - cp.p[c], cp.y[3][c] - cp.p[c]);
        float2 sg = -cp.sigma;
        float ac = cp.a[c], bc = cp.b[c];
        if (MAGNITUDE)
        {
            T = bAbs(T);
            B = bAbs(B);
            N = bAbs(N);
            tx = bAbs(tx);
            ty = bAbs(ty);
            tz = bAbs(tz);
            d = bBilinear(abs(cp.y[0][c]) + abs(cp.p[c]), abs(cp.y[1][c]) + abs(cp.p[c]), abs(cp.y[2][c]) + abs(cp.p[c]),
                          abs(cp.y[3][c]) + abs(cp.p[c]));
            sg = abs(sg);
            ac = abs(ac);
            bc = abs(bc);
        }
        const BPoly h = bAxpy(bAxpy(bAxpy(bMul<1, 1>(tx, T), 1.0, bMul<1, 1>(ty, B)), 1.0, bMul<1, 1>(tz, N)), 1.0,
                              bElevate(bAxpy(bAxpy(bZero(1, 1), sg.x, T), sg.y, B)));
        hh = bAxpy(hh, 1.0, bMul<2, 2>(h, h));
        dh = bAxpy(dh, 1.0, bMul<1, 2>(d, h));
        ha = bAxpy(ha, ac, h);
        hb = bAxpy(hb, bc, h);
        da = bAxpy(da, ac, d);
        db = bAxpy(db, bc, d);
    }
    const float two = MAGNITUDE ? 2.0 : -2.0;
    F1 = bAxpy(bMul<1, 4>(da, hh), two, bMul<3, 2>(dh, ha));
    F2 = bAxpy(bMul<1, 4>(db, hh), two, bMul<3, 2>(dh, hb));
}

// The grids' rounding bounds (mu): kPolyRoundings roundings of each coefficient's terms' magnitude. The solver does not
// need them (it solves the grids, "What is solved exactly" above); the CPU twin's tests measure them.
void cellPolyRounding(inout CellPoly cp)
{
    BPoly A1, A2;
    cellPolyGrids<true>(cp, A1, A2);
    cp.mu1 = kPolyRoundings * kPolyUlp * bMaxAbs(A1);
    cp.mu2 = kPolyRoundings * kPolyUlp * bMaxAbs(A2);
}

// What a solve is given: the triangle (its frame per vertex, interpolated as it is), the cell's four texels' tangent-space
// normals at (s, t) = (0, 0), (1, 0), (0, 1), (1, 1), the cell's texture coordinates (its (0, 0) and a texel's size), the
// receiver p, the sun's direction L and the slope. The solver builds the cell's grids from it, and a part's (its bases).
struct PolyCellSource
{
    ManifoldMappedTri m;
    float3 c[4];
    float2 origin, size;
    float3 p, L;
    float2 sigma;
}

// A normal-mapped triangle's cell.
PolyCellSource polyCellSource(ManifoldMappedTri m, float3 c00, float3 c10, float3 c01, float3 c11, float2 origin,
                              float2 size, float3 p, float3 L, float2 sigma)
{
    PolyCellSource src;
    src.m = m;
    src.c[0] = c00;
    src.c[1] = c10;
    src.c[2] = c01;
    src.c[3] = c11;
    src.origin = origin;
    src.size = size;
    src.p = p;
    src.L = L;
    src.sigma = sigma;
    return src;
}

// A plain triangle as a cell whose texels are all (0, 0, 1) (one solver for both): the cell is the triangle's own (u, v)
// square, of which the triangle is the half u + v <= 1, and the frame at each vertex is 2a's there (rt/manifold.slang's
// frame of reference: the unit normal turned to the side the triangle shows p, t = normalize(c - (c . n) n), b = n x t),
// interpolated as it is (vT and vB are -t and -b: h = n + sigma.x t + sigma.y b, 2a's sign of the slope). h is then the
// blend, at the barycentric weights, of the three vertices' 2a half vectors for sigma, each of length sqrt(1 + |sigma|^2),
// so its direction lies in every cone that holds those three: 2a's cone and disc tests, which bound the half vector a
// slope means over a node's normals by such a cone, keep every triangle with a root in this frame. One sign for the
// triangle (its summed normal's side). Where c lies within 0.04 of a vertex's normal (2a's frame undefined there), the
// disc tests keep the triangle whatever its frame (manifoldDiscTerm: within 0.05 rad they do not prune), and the frame
// takes a c perpendicular to the summed normal instead. False when the frame is undefined at a vertex.
bool polyTriangleSource(ManifoldTri tri, float3 p, float3 L, float2 sigma, out PolyCellSource src)
{
    const float3 g = cross(tri.e1, tri.e2);
    const float side = dot(g, p - tri.p0) >= 0.0 ? 1.0 : -1.0;
    float3 u[3] = { tri.n0, tri.n0 + tri.a1, tri.n0 + tri.a2 };
    bool ok = true;
    for (uint k = 0u; k < 3u; ++k)
    {
        const float l = length(u[k]);
        ok = ok && l > 1e-12;
        u[k] = l > 1e-12 ? u[k] / l : float3(0.0, 0.0, 1.0);
    }
    float3 sum = u[0] + u[1] + u[2];
    if (!(dot(sum, sum) > 1e-12)) sum = g;
    const float turn = dot(sum, g) * side < 0.0 ? -1.0 : 1.0;
    float3 c = tri.c;
    float near = 1.0;
    for (uint k = 0u; k < 3u; ++k)
    {
        u[k] *= turn;
        near = min(near, length(cross(c, u[k])));
    }
    if (!(near >= 0.04))
    {
        const float3 a = normalize(sum);
        c = normalize(abs(a.x) < 0.7 ? cross(a, float3(1.0, 0.0, 0.0)) : cross(a, float3(0.0, 1.0, 0.0)));
    }
    float3 t[3], b[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        const float3 T = c - u[k] * dot(c, u[k]);
        const float lt = length(T);
        ok = ok && lt > 1e-6;
        t[k] = lt > 1e-6 ? T / lt : float3(1.0, 0.0, 0.0);
        b[k] = cross(u[k], t[k]);
    }
    ManifoldMappedTri m;
    m.tri = tri;
    m.tri.n0 = u[0];
    m.tri.a1 = u[1] - u[0];
    m.tri.a2 = u[2] - u[0];
    m.t0 = -t[0];
    m.ta1 = t[0] - t[1];
    m.ta2 = t[0] - t[2];
    m.b0 = -b[0];
    m.ba1 = b[0] - b[1];
    m.ba2 = b[0] - b[2];
    m.uv0 = float2(0.0);
    m.uvA = float2(1.0, 0.0);
    m.uvB = float2(0.0, 1.0);
    const float3 z = float3(0.0, 0.0, 1.0);
    src = polyCellSource(m, z, z, z, z, float2(0.0), float2(1.0), p, L, sigma);
    return ok;
}

// The surface's area a unit of slope covers at a root st, |dA / d sigma| = |dA / d(s, t)| / |det d sigma / d(s, t)|, the
// estimator's change of variables (any frame: the root's density is the slope's times this). F(s, t, sigma(s, t)) = 0
// where sigma(s, t) is the slope whose h lies along the half vector there, so d sigma / d(s, t) = -(dF / d sigma)^-1
// dF / d(s, t): dF / d(s, t) the derivative grids at st; dF / d sigma through h, dh / d sigma = -(vT, vB), and the change
// of r = d (h . h) - 2 (d . h) h with h by e, 2 (h . e) d - 2 (d . e) h - 2 (d . h) e. areaST: the surface's area a unit
// of (s, t), |e1 x e2| |det (ds, dt)|. 0 where dF / d(s, t) is singular.
float cellPolyAreaPerSlope(CellPoly cp, float2 st, float areaST)
{
    const float j11 = bEval<4, 5>(cp.D1s, st.x, st.y), j12 = bEval<5, 4>(cp.D1t, st.x, st.y);
    const float j21 = bEval<4, 5>(cp.D2s, st.x, st.y), j22 = bEval<5, 4>(cp.D2t, st.x, st.y);
    const float detST = j11 * j22 - j12 * j21;
    float3 h, y;
    polyReflectAt(cp, st, h, y);
    const float3 d = y - cp.p;
    const float3 t = polyCorner(cp.vT, st), b = polyCorner(cp.vB, st);
    const float dh = dot(d, h);
    const float3 rx = 2.0 * (dot(d, t) * h + dh * t - dot(h, t) * d);
    const float3 ry = 2.0 * (dot(d, b) * h + dh * b - dot(h, b) * d);
    const float detS = dot(rx, cp.a) * dot(ry, cp.b) - dot(ry, cp.a) * dot(rx, cp.b);
    if (!(abs(detST) > 0.0)) return 0.0;
    return areaST * abs(detS) / abs(detST);
}

bool polyOnTriangle(CellPoly cp, float2 st)
{
    const float2 uv = cp.o + cp.ds * st.x + cp.dt * st.y;
    return uv.x >= 0.0 && uv.y >= 0.0 && uv.x + uv.y <= 1.0;
}

// A box wholly off the triangle ((u, v) affine in the box: its extremes at the corners), with a margin for rounding.
bool polyBoxOffTriangle(CellPoly cp, float4 box)
{
    const float2 a = cp.o + cp.ds * box.x + cp.dt * box.y, b = cp.o + cp.ds * box.z + cp.dt * box.y;
    const float2 c = cp.o + cp.ds * box.x + cp.dt * box.w, d = cp.o + cp.ds * box.z + cp.dt * box.w;
    const float2 hi = max(max(a, b), max(c, d));
    const float sumLo = min(min(a.x + a.y, b.x + b.y), min(c.x + c.y, d.x + d.y));
    return hi.x < -1e-6 || hi.y < -1e-6 || sumLo > 1.0 + 1e-6;
}

// A root of F is a path: in the cell, on the triangle, r . L > 0 and h . H > 0.
bool polyPhysical(CellPoly cp, float2 st)
{
    if (any(st < 0.0) || any(st > 1.0) || !polyOnTriangle(cp, st)) return false;
    float3 h, y;
    const float3 r = polyReflectAt(cp, st, h, y);
    const float3 toP = cp.p - y;
    const float lp = length(toP);
    if (!(lp > 0.0)) return false;
    return dot(r, cp.L) > 0.0 && dot(h, toP / lp + cp.L) > 0.0;
}

struct PolyCellResult
{
    uint roots;       // paths found (at most kPolyMaxRoots kept; more counted in overflow)
    uint undecided;   // boxes left undecided (the budget spent, or narrower than kPolyMinWidth)
    uint boxes;       // boxes tested
    uint overflow;
    float2 st[8];     // the roots, as (s, t) in the cell
    float w[8];       // the area a unit of slope covers at each (cellPolyAreaPerSlope)
}

// Krawczyk's test of F on a box (cell coordinates). Returns -1: no root in it, 1: exactly one, at `root` (simplified
// Newton from the centre, which K inside the box guarantees converges), 0: undecided; `next` the box K leaves (K
// intersected with the box) when undecided, `blur` the part of K's radius the rounding alone makes (|Y| times F's
// error): no box narrower than it can be certified; `centre` K's centre (c - Y F(c): Newton's estimate of a root).
int polyKrawczyk(CellPoly cp, float4 box, out float2 root, out float4 next, out float2 blur, out float2 centre)
{
    root = float2(0.0);
    next = box;
    blur = float2(0.0);
    centre = 0.5 * (box.xy + box.zw);
    const float2 w = box.zw - box.xy;
    const BPoly G1 = bBox<5, 5>(cp.F1, box), G2 = bBox<5, 5>(cp.F2, box);
    // A box's extraction rounds by m in its coefficients (past [0, 1], a grown box's extrapolation, by up to
    // (1 + 2 kPolyGrow)^5 < 3 times that). The Jacobian's grids are extracted from the cell's derivatives, so their
    // rounding is absolute too: differenced from the box's own grid it was 2 n m over the box's width, which grew as
    // boxes shrank until no box near a root could contract (m12, 45 degrees).
    const float ex = 3.0 * kPolyExtractRoundings * kPolyUlp;
    const float m1 = ex * cp.mF.x, m2 = ex * cp.mF.y;
    float lo1, hi1, lo2, hi2;
    bHull(G1, lo1, hi1);
    bHull(G2, lo2, hi2);
    if (lo1 - m1 > 0.0 || hi1 + m1 < 0.0 || lo2 - m2 > 0.0 || hi2 + m2 < 0.0) return -1;
    // The Jacobian over the box (cell units): each derivative's grid over the box, its hull with its rounding.
    const BPoly D1s = bBox<4, 5>(cp.D1s, box), D1t = bBox<5, 4>(cp.D1t, box), D2s = bBox<4, 5>(cp.D2s, box);
    const BPoly D2t = bBox<5, 4>(cp.D2t, box);
    float4 jLo, jHi;   // (dF1/ds, dF1/dt, dF2/ds, dF2/dt)
    bHull(D1s, jLo.x, jHi.x);
    bHull(D1t, jLo.y, jHi.y);
    bHull(D2s, jLo.z, jHi.z);
    bHull(D2t, jLo.w, jHi.w);
    const float4 jMu = ex * cp.mD;
    jLo -= jMu;
    jHi += jMu;
    // At the centre: F on the cell's grid (de Casteljau: 10 levels of convex combinations, 12 roundings of its largest
    // coefficient, 3 times that past [0, 1]), not the box's (whose extraction rounds 6 times as much: K's blur near a fold
    // was wider than any box that could contract there); the Jacobian, Y its inverse.
    const float2 c = 0.5 * (box.xy + box.zw);
    const float fx = (all(c >= 0.0) && all(c <= 1.0) ? 12.0 : 36.0) * kPolyUlp;
    const float e1 = fx * cp.mF.x, e2 = fx * cp.mF.y;
    const float f1 = bEval<5, 5>(cp.F1, c.x, c.y), f2 = bEval<5, 5>(cp.F2, c.x, c.y);
    const float4 jc = float4(bEval<4, 5>(D1s, 0.5, 0.5), bEval<5, 4>(D1t, 0.5, 0.5), bEval<4, 5>(D2s, 0.5, 0.5),
                             bEval<5, 4>(D2t, 0.5, 0.5));
    const float det = jc.x * jc.w - jc.y * jc.z;
    const float jn = max(max(abs(jc.x), abs(jc.y)), max(abs(jc.z), abs(jc.w)));
    if (!(abs(det) > 1e-12 * jn * jn)) return 0;
    const float2x2 Y = float2x2(jc.w, -jc.y, -jc.z, jc.x) / det;
    const float2 r = 0.5 * w;
    // K = c - Y F(c) + (I - Y J(B)) (B - c), with F(c) +- e and B - c = [-r, r].
    const float2 yf = mul(Y, float2(f1, f2));
    float2 rad = float2(abs(Y[0][0]) * e1 + abs(Y[0][1]) * e2, abs(Y[1][0]) * e1 + abs(Y[1][1]) * e2);
    blur = rad;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
        {
            // (Y J(B))_ij = Y_i0 J_0j + Y_i1 J_1j over the intervals; (I - Y J(B))_ij's magnitude times r_j.
            const float y0 = Y[i][0], y1 = Y[i][1];
            const float a0 = j == 0 ? jLo.x : jLo.y, b0 = j == 0 ? jHi.x : jHi.y;
            const float a1 = j == 0 ? jLo.z : jLo.w, b1 = j == 0 ? jHi.z : jHi.w;
            const float lo = min(y0 * a0, y0 * b0) + min(y1 * a1, y1 * b1);
            const float hi = max(y0 * a0, y0 * b0) + max(y1 * a1, y1 * b1);
            const float dij = i == j ? 1.0 : 0.0;
            rad[i] += max(abs(dij - lo), abs(dij - hi)) * r[j];
        }
    const float2 kc = c - yf;
    centre = kc;
    // The few roundings of K's own arithmetic.
    rad += 16.0 * kPolyUlp * (abs(c) + abs(yf) + rad);
    const float2 kLo = kc - rad, kHi = kc + rad;
    if (any(kHi < box.xy) || any(kLo > box.zw)) return -1;
    if (all(kLo > box.xy) && all(kHi < box.zw))
    {
        // One root; simplified Newton with this Y converges to it inside the box.
        float2 x = c;
        for (int it = 0; it < 40; ++it)
        {
            const float2 l = (x - box.xy) / w;
            const float2 dx = mul(Y, float2(bEval<5, 5>(G1, l.x, l.y), bEval<5, 5>(G2, l.x, l.y)));
            x -= dx;
            if (all(abs(dx) <= 1e-7 * max(w, float2(1e-30)))) break;
        }
        root = clamp(x, box.xy, box.zw);
        return 1;
    }
    next = float4(max(box.xy, kLo), min(box.zw, kHi));
    return 0;
}

// A certified root (local: in the grids' own box; st: in the cell), kept if it is a path and not already kept (a root
// another base's grids certified beside a seam, within their rounding).
void polyKeep(CellPoly cp, float2 local, float2 st, float areaST, inout PolyCellResult res)
{
    if (!polyPhysical(cp, local)) return;
    for (uint q = 0u; q < min(res.roots, kPolyMaxRoots); ++q)
        if (all(abs(res.st[q] - st) <= 1e-5)) return;
    if (res.roots < kPolyMaxRoots)
    {
        res.st[res.roots] = st;
        res.w[res.roots] = cellPolyAreaPerSlope(cp, local, areaST);
    }
    else ++res.overflow;
    ++res.roots;
}

bool polyInHalfOpen(float2 x, float4 box) { return all(x >= box.xy) && all(x < box.zw); }

// Every root of the cell's constraint that is a path (polyPhysical), by subdivision. A box is tested grown by kPolyGrow
// a side (a root on a split line lies inside some grown box): excluded, or its one root certified, or K leaves part of
// the grown box. Every root of the box lies in what K leaves, so a box round that part, at least 1.5 times as wide as
// it and as the rounding's blur (Rump's epsilon inflation: K never shrinks below the blur, and halves narrower than it
// never certify), is tested next: excluded, or certified, it settles the box. Round what K leaves of the grown box, not
// of the box: beside a root another box certifies, the grown box holds it and K leaves a sliver of the box along it,
// whose own box misses the root and never decides (m12: 12 of 398 cells split to the minimum width). The box also
// covers K's centre (Newton's estimate) by twice the blur: centred on a sliver 1.3e-5 from a root whose K is 2e-5 wide,
// K poked out of it. Else what K left of the box is split along its wider side. Depth first. A certified root is kept
// by the region the test settles, half-open (the box, or the halves under an inflation box): a root in a grown margin
// is its neighbour's, which finds it, so no root is kept twice.
//
// Bases. A box's grids are extracted from its base's (two de Casteljau passes a direction), whose rounding is absolute,
// a few ulps of the base grid's largest coefficient: on a large cell (a plain triangle is one) that margin can exceed F
// itself beside a fold (two roots near merging), where no box then excludes or certifies, at any width (the CPU's
// random triangles: 3 of 2,000 spent 100,000 boxes so). So the grids are rebuilt from the source on smaller squares: a
// part of the box K leaves, once it is no wider than kPolyRebase of its base, goes to the dyadic squares of the next
// level (an eighth of its base's side, at most kPolyLevels levels), each on grids of its own, whose margins scale with
// F's range there. Grids differ between bases by their construction's rounding (as between neighbouring cells); a root
// beside a seam is kept once (polyKeep).
//
// Resumable: the stack is the whole state, so a solve runs a budget of tests at a time (cellPolySolveSteps) and the
// next call goes on where it stopped; finished when the stack is empty. A stack entry is a box in the cell's (s, t)
// with a word: its kind (bits 0-1: 0 a box of the partition, tested grown; 1 + n an inflation box, tested as it is,
// the n entries under it the halves of what K left, dropped when it settles the box, tested when it does not) and its
// base (bits 2-3 the level l, 4-12 and 13-21 the square's i, j: [i, i + 1] x [j, j + 1] / 8^l).
static const uint kPolyLevels = 3u;
static const float kPolyRebase = 0.125;

struct PolySolveState
{
    float4 stack[kPolyStack];
    uint kinds[kPolyStack];
    uint top;
}

PolySolveState cellPolySolveStart()
{
    PolySolveState s;
    for (uint k = 0u; k < kPolyStack; ++k)
    {
        s.stack[k] = float4(0.0);
        s.kinds[k] = 0u;
    }
    s.stack[0] = float4(0.0, 0.0, 1.0, 1.0);
    s.top = 1u;
    return s;
}

PolyCellResult cellPolyResultStart()
{
    PolyCellResult res;
    res.roots = 0u;
    res.undecided = 0u;
    res.boxes = 0u;
    res.overflow = 0u;
    for (uint k = 0u; k < kPolyMaxRoots; ++k)
    {
        res.st[k] = float2(0.0);
        res.w[k] = 0.0;
    }
    return res;
}

// A base's square in the cell: (its origin, its side).
float3 polyBaseSquare(uint base)
{
    const float side = 1.0 / float(1u << (3u * (base & 3u)));
    return float3(float((base >> 2u) & 511u), float((base >> 11u) & 511u), 1.0) * side;
}

// At most `budget` tests (a Krawczyk test or a base's grids built; one call site each: each is compiled whole) from where
// the state stands; res accumulates the roots, the boxes tested and the undecided ones. A step pops one entry and
// pushes at most four after a test, so the loop ends within kPolyStack + 4 (budget + 1) steps.
void cellPolySolveSteps(PolyCellSource src, inout PolySolveState s, inout PolyCellResult res, uint budget)
{
    CellPoly cp;
    uint built = 0xFFFFFFFFu;   // the base cp holds (none yet)
    bool usable = false;
    float3 sq = float3(0.0, 0.0, 1.0);
    float areaST = 0.0;
    const float area2 = length(cross(src.m.tri.e1, src.m.tri.e2));
    uint tests = 0u;
    for (uint step = 0u; step < kPolyStack + 4u * (budget + 1u); ++step)
    {
        if (s.top == 0u || tests >= budget) break;
        --s.top;
        const float4 box = s.stack[s.top];
        const uint word = s.kinds[s.top];
        const uint kind = word & 3u, base = word >> 2u;
        if (base != built)
        {
            // The base's grids, from the source at its square's corners (the texels' bilinear blend there).
            ++tests;
            built = base;
            sq = polyBaseSquare(base);
            float3 c[4];
            for (uint k = 0u; k < 4u; ++k) c[k] = polyCorner(src.c, sq.xy + sq.z * float2(float(k & 1u), float(k >> 1u)));
            usable = cellPolyOf(src.m, c[0], c[1], c[2], c[3], src.origin + src.size * sq.xy, src.size * sq.z, src.p,
                                src.L, src.sigma, cp);
            areaST = area2 * abs(cp.ds.x * cp.dt.y - cp.ds.y * cp.dt.x);
        }
        if (!usable)
        {
            ++res.undecided;
            continue;
        }
        const float4 lb = (box - sq.xyxy) / sq.z;   // the box in the base's own (s, t)
        if (kind == 0u && polyBoxOffTriangle(cp, lb)) continue;
        ++tests;
        ++res.boxes;
        const float2 g = kind == 0u ? kPolyGrow * (lb.zw - lb.xy) : float2(0.0);
        float2 root, blur, centre;
        float4 next;
        const int k = polyKrawczyk(cp, float4(lb.xy - g, lb.zw + g), root, next, blur, centre);
        if (k != 0)
        {
            if (k > 0)
            {
                // The region the test settles: the box, or the halves under the inflation box (itself without them).
                const float2 st = sq.xy + root * sq.z;
                const bool mine = kind == 0u ? polyInHalfOpen(st, box)
                                  : kind == 1u ? all(st >= box.xy) && all(st <= box.zw)
                                               : polyInHalfOpen(st, s.stack[s.top - 1u]) ||
                                                     (kind == 3u && polyInHalfOpen(st, s.stack[s.top - 2u]));
                if (mine) polyKeep(cp, root, st, areaST, res);
            }
            if (kind > 0u) s.top -= kind - 1u;   // the halves under an inflation box that settled its box
            continue;
        }
        if (kind > 0u)
        {
            if (kind == 1u) ++res.undecided;   // nothing under it: what K left could not be split
            continue;
        }
        // Undecided: what K leaves of the box (every root of the box is in it), in the cell's (s, t).
        const float4 nextC = sq.xyxy + next * sq.z;
        const float4 rest = float4(max(nextC.xy, box.xy), min(nextC.zw, box.zw));
        const float2 rw = rest.zw - rest.xy;
        if (any(rw < 0.0)) continue;
        const uint level = base & 3u;
        if (level < kPolyLevels && max(rw.x, rw.y) <= kPolyRebase * sq.z)
        {
            // Its parts in the next level's squares (at most two a side), each on its own grids.
            const float n = float(1u << (3u * (level + 1u)));
            const int2 lo = int2(floor(rest.xy * n)), hi = int2(floor(rest.zw * n));
            if (s.top + 4u > kPolyStack)
            {
                ++res.undecided;
                continue;
            }
            for (uint q = 0u; q < 4u; ++q)
            {
                const int2 ij = lo + int2(int(q & 1u), int(q >> 1u));
                if (ij.x > hi.x || ij.y > hi.y) continue;
                const float4 part = float4(max(rest.xy, float2(ij) / n), min(rest.zw, float2(ij + 1) / n));
                if (!all(part.zw > part.xy)) continue;
                s.stack[s.top] = part;
                s.kinds[s.top++] = (level + 1u | uint(ij.x) << 2u | uint(ij.y) << 11u) << 2u;
            }
            continue;
        }
        // Else split along its wider side, under an inflation box round what K leaves of the grown box and K's centre
        // by twice the blur.
        uint halves = 0u;
        if (max(rw.x, rw.y) >= kPolyMinWidth * sq.z && s.top + 3u <= kPolyStack)
        {
            const bool alongS = rw.x >= rw.y;
            const float m = alongS ? rest.x + 0.5 * rw.x : rest.y + 0.5 * rw.y;
            s.stack[s.top] = alongS ? float4(rest.x, rest.y, m, rest.w) : float4(rest.x, rest.y, rest.z, m);
            s.kinds[s.top++] = word & ~3u;
            s.stack[s.top] = alongS ? float4(m, rest.y, rest.z, rest.w) : float4(rest.x, m, rest.z, rest.w);
            s.kinds[s.top++] = word & ~3u;
            halves = 2u;
        }
        if (s.top + 1u > kPolyStack)
        {
            ++res.undecided;
            continue;
        }
        const float2 lo = min(next.xy, centre - 2.0 * blur), hi = max(next.zw, centre + 2.0 * blur);
        const float2 g2 = 0.25 * (hi - lo) + kPolyMinWidth;
        s.stack[s.top] = sq.xyxy + float4(lo - g2, hi + g2) * sq.z;
        s.kinds[s.top++] = (word & ~3u) | (1u + halves);
    }
}
