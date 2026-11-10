// Step 2c's cell solver (docs/specs/2026-11-04-caustics-step2-design.md, "Step 2c", T2; rulings M11b, M13; the plan
// docs/plans/2026-11-08-caustics-step2c.md, tasks 1 and 2): on one cell of a normal map (the square between four texel
// centres, (s, t) in [0, 1]^2) of one triangle, the reflection constraint as two polynomials in tensor Bernstein form,
// and every root of them by subdivision and Krawczyk's test. A module (no entry point): tools/caustics/manifold_kernel
// .slang imports it (the CPU twin).
//
// The polynomials. MikkTSpace's mapped normal (ruling M13) from the interpolated vectors as they are, vB the
// interpolation of each vertex's sign N x T, and the map's four texels blended bilinearly:
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

// A tensor Bernstein grid of degree (ns, nt) <= (5, 5): c[i * (nt + 1) + j], i along s, j along t.
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
    r.c[2] = f10;
    r.c[3] = f11;
    return r;
}

// The product: degree (a.ns + b.ns, a.nt + b.nt), each coefficient a convex combination of products.
BPoly bMul(BPoly a, BPoly b)
{
    BPoly r = bZero(a.ns + b.ns, a.nt + b.nt);
    for (int i = 0; i <= a.ns; ++i)
        for (int k = 0; k <= b.ns; ++k)
        {
            const float ws = polyBinom(a.ns, i) * polyBinom(b.ns, k) / polyBinom(r.ns, i + k);
            for (int j = 0; j <= a.nt; ++j)
                for (int l = 0; l <= b.nt; ++l)
                {
                    const float w = ws * polyBinom(a.nt, j) * polyBinom(b.nt, l) / polyBinom(r.nt, j + l);
                    r.c[(i + k) * (r.nt + 1) + j + l] += w * a.c[i * (a.nt + 1) + j] * b.c[k * (b.nt + 1) + l];
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

// Raised to degree (ns, nt) (>= its own): the product with the constant 1 of the difference.
BPoly bElevate(BPoly a, int ns, int nt)
{
    if (a.ns == ns && a.nt == nt) return a;
    BPoly one = bZero(ns - a.ns, nt - a.nt);
    for (int k = 0; k < 36; ++k) one.c[k] = 1.0;
    return bMul(a, one);
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
    for (int k = 0; k < (a.ns + 1) * (a.nt + 1); ++k) m = max(m, abs(a.c[k]));
    return m;
}

void bHull(BPoly a, out float lo, out float hi)
{
    lo = a.c[0];
    hi = a.c[0];
    for (int k = 1; k < (a.ns + 1) * (a.nt + 1); ++k)
    {
        lo = min(lo, a.c[k]);
        hi = max(hi, a.c[k]);
    }
}

// The value at (s, t) by de Casteljau (convex combinations inside [0, 1]^2).
float bEval(BPoly a, float s, float t)
{
    float col[6];
    for (int i = 0; i <= a.ns; ++i)
    {
        float w[6];
        for (int j = 0; j <= a.nt; ++j) w[j] = a.c[i * (a.nt + 1) + j];
        for (int q = 1; q <= a.nt; ++q)
            for (int j = 0; j <= a.nt - q; ++j) w[j] = (1.0 - t) * w[j] + t * w[j + 1];
        col[i] = w[0];
    }
    for (int q = 1; q <= a.ns; ++q)
        for (int i = 0; i <= a.ns - q; ++i) col[i] = (1.0 - s) * col[i] + s * col[i + 1];
    return col[0];
}

// One row's Bernstein coefficients (degree n) on [0, 1] to those of the same polynomial on [a0, a1]: de Casteljau at a0
// keeping the right part, then at (a1 - a0) / (1 - a0) keeping the left.
void bRange1(inout float w[6], int n, float a0, float a1)
{
    float t[6];
    float right[6];
    for (int k = 0; k < 6; ++k)
    {
        t[k] = w[k];
        right[k] = w[k];
    }
    for (int q = 1; q <= n; ++q)
    {
        for (int j = 0; j <= n - q; ++j) t[j] = (1.0 - a0) * t[j] + a0 * t[j + 1];
        right[n - q] = t[n - q];
    }
    const float tau = (a1 - a0) / (1.0 - a0);
    float u[6];
    for (int k = 0; k < 6; ++k) u[k] = right[k];
    w[0] = right[0];
    for (int q = 1; q <= n; ++q)
    {
        for (int j = 0; j <= n - q; ++j) u[j] = (1.0 - tau) * u[j] + tau * u[j + 1];
        w[q] = u[0];
    }
}

// The grid of the same polynomial over the box [s0, s1] x [t0, t1] of [0, 1]^2 (s0 < 1, t0 < 1).
BPoly bBox(BPoly a, float4 box)
{
    BPoly r = a;
    for (int j = 0; j <= a.nt; ++j)
    {
        float w[6];
        for (int i = 0; i < 6; ++i) w[i] = i <= a.ns ? r.c[i * (a.nt + 1) + j] : 0.0;
        bRange1(w, a.ns, box.x, box.z);
        for (int i = 0; i <= a.ns; ++i) r.c[i * (a.nt + 1) + j] = w[i];
    }
    for (int i = 0; i <= a.ns; ++i)
    {
        float w[6];
        for (int j = 0; j < 6; ++j) w[j] = j <= a.nt ? r.c[i * (a.nt + 1) + j] : 0.0;
        bRange1(w, a.nt, box.y, box.w);
        for (int j = 0; j <= a.nt; ++j) r.c[i * (a.nt + 1) + j] = w[j];
    }
    return r;
}

// The derivative along s (axis 0) or t (axis 1) in the grid's own parameters: degree one lower along it.
BPoly bDerivative(BPoly a, int axis)
{
    BPoly r = bZero(axis == 0 ? a.ns - 1 : a.ns, axis == 0 ? a.nt : a.nt - 1);
    for (int i = 0; i <= r.ns; ++i)
        for (int j = 0; j <= r.nt; ++j)
        {
            const float hi = axis == 0 ? a.c[(i + 1) * (a.nt + 1) + j] : a.c[i * (a.nt + 1) + j + 1];
            r.c[i * (r.nt + 1) + j] = float(axis == 0 ? a.ns : a.nt) * (hi - a.c[i * (a.nt + 1) + j]);
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
    float3 y[4], vN[4], vT[4], vB[4], tn[4];
    float3 p, L, a, b;
    float2 sigma;
    float2 o, ds, dt;          // the triangle's (u, v) = o + ds s + dt t
}

float3 polyCorner(float3 c[4], float2 st)
{
    return lerp(lerp(c[0], c[1], st.x), lerp(c[2], c[3], st.x), st.y);
}

// MikkTSpace's frame at the triangle's (u, v): the interpolated normal, tangent and bitangent as they are.
void polyFrameAt(ManifoldMappedTri m, float2 uv, out float3 n, out float3 t, out float3 b)
{
    const ManifoldTri tri = m.tri;
    n = tri.n0 + tri.a1 * uv.x + tri.a2 * uv.y;
    t = m.t0 + m.ta1 * uv.x + m.ta2 * uv.y;
    const float sg = m.sign < 0.0 ? -1.0 : 1.0;
    const float3 b0 = sg * cross(tri.n0, m.t0);
    const float3 b1 = sg * cross(tri.n0 + tri.a1, m.t0 + m.ta1);
    const float3 b2 = sg * cross(tri.n0 + tri.a2, m.t0 + m.ta2);
    b = b0 + (b1 - b0) * uv.x + (b2 - b0) * uv.y;
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
    const float det = m.uvA.x * m.uvB.y - m.uvB.x * m.uvA.y;
    if (!(abs(det) > 1e-20)) return false;
    const float2x2 inv = float2x2(m.uvB.y, -m.uvB.x, -m.uvA.y, m.uvA.x) / det;
    cp.o = mul(inv, origin - m.uv0);
    cp.ds = float2(inv[0][0], inv[1][0]) * size.x;
    cp.dt = float2(inv[0][1], inv[1][1]) * size.y;
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
        const BPoly h = bAxpy(bAxpy(bAxpy(bMul(tx, T), 1.0, bMul(ty, B)), 1.0, bMul(tz, N)), 1.0,
                              bElevate(bAxpy(bAxpy(bZero(1, 1), sg.x, T), sg.y, B), 2, 2));
        hh = bAxpy(hh, 1.0, bMul(h, h));
        dh = bAxpy(dh, 1.0, bMul(d, h));
        ha = bAxpy(ha, ac, h);
        hb = bAxpy(hb, bc, h);
        da = bAxpy(da, ac, d);
        db = bAxpy(db, bc, d);
    }
    const float two = MAGNITUDE ? 2.0 : -2.0;
    F1 = bAxpy(bMul(da, hh), two, bMul(dh, ha));
    F2 = bAxpy(bMul(db, hh), two, bMul(dh, hb));
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
    const BPoly G1 = bBox(cp.F1, box), G2 = bBox(cp.F2, box);
    // A box's extraction rounds by m in its coefficients (past [0, 1], a grown box's extrapolation, by up to
    // (1 + 2 kPolyGrow)^5 < 3 times that). The Jacobian's grids are extracted from the cell's derivatives, so their
    // rounding is absolute too: differenced from the box's own grid it was 2 n m over the box's width, which grew as
    // boxes shrank until no box near a root could contract (m12, 45 degrees).
    const float ex = 3.0 * kPolyExtractRoundings * kPolyUlp;
    const float m1 = ex * bMaxAbs(cp.F1), m2 = ex * bMaxAbs(cp.F2);
    float lo1, hi1, lo2, hi2;
    bHull(G1, lo1, hi1);
    bHull(G2, lo2, hi2);
    if (lo1 - m1 > 0.0 || hi1 + m1 < 0.0 || lo2 - m2 > 0.0 || hi2 + m2 < 0.0) return -1;
    // The Jacobian over the box (cell units): each derivative's grid over the box, its hull with its rounding.
    const BPoly D1s = bBox(cp.D1s, box), D1t = bBox(cp.D1t, box), D2s = bBox(cp.D2s, box), D2t = bBox(cp.D2t, box);
    float4 jLo, jHi;   // (dF1/ds, dF1/dt, dF2/ds, dF2/dt)
    bHull(D1s, jLo.x, jHi.x);
    bHull(D1t, jLo.y, jHi.y);
    bHull(D2s, jLo.z, jHi.z);
    bHull(D2t, jLo.w, jHi.w);
    const float4 jMu = ex * float4(bMaxAbs(cp.D1s), bMaxAbs(cp.D1t), bMaxAbs(cp.D2s), bMaxAbs(cp.D2t));
    jLo -= jMu;
    jHi += jMu;
    // At the centre: F on the cell's grid (de Casteljau: 10 levels of convex combinations, 12 roundings of its largest
    // coefficient, 3 times that past [0, 1]), not the box's (whose extraction rounds 6 times as much: K's blur near a fold
    // was wider than any box that could contract there); the Jacobian, Y its inverse.
    const float2 c = 0.5 * (box.xy + box.zw);
    const float fx = (all(c >= 0.0) && all(c <= 1.0) ? 12.0 : 36.0) * kPolyUlp;
    const float e1 = fx * bMaxAbs(cp.F1), e2 = fx * bMaxAbs(cp.F2);
    const float f1 = bEval(cp.F1, c.x, c.y), f2 = bEval(cp.F2, c.x, c.y);
    const float4 jc = float4(bEval(D1s, 0.5, 0.5), bEval(D1t, 0.5, 0.5), bEval(D2s, 0.5, 0.5), bEval(D2t, 0.5, 0.5));
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
            const float2 dx = mul(Y, float2(bEval(G1, l.x, l.y), bEval(G2, l.x, l.y)));
            x -= dx;
            if (all(abs(dx) <= 1e-7 * max(w, float2(1e-30)))) break;
        }
        root = clamp(x, box.xy, box.zw);
        return 1;
    }
    next = float4(max(box.xy, kLo), min(box.zw, kHi));
    return 0;
}

// A certified root, kept if it is a path and not already kept (another box may certify the same root).
void polyKeep(CellPoly cp, float2 root, inout PolyCellResult res)
{
    if (!polyPhysical(cp, root)) return;
    for (uint q = 0u; q < min(res.roots, kPolyMaxRoots); ++q)
        if (all(abs(res.st[q] - root) <= 1e-5)) return;
    if (res.roots < kPolyMaxRoots) res.st[res.roots] = root;
    else ++res.overflow;
    ++res.roots;
}

// Every root of the cell's constraint that is a path (polyPhysical), by subdivision. A box is tested grown by kPolyGrow
// a side (a root on a split line lies inside some grown box): excluded, or its one root certified, or K leaves part of
// the grown box. Every root of the box lies in what K leaves, so a box round that part, at least 1.5 times as wide as
// it and as the rounding's blur (Rump's epsilon inflation: K never shrinks below the blur, and halves narrower than it
// never certify), is tested next: excluded, or certified, it settles the box (the root it certifies may lie outside
// the box: a neighbour's, kept once). Round what K leaves of the grown box, not of the box: beside a root another box
// certifies, the grown box holds it and K leaves a sliver of the box along it, whose own box misses the root and
// never decides (m12: 12 of 398 cells split to the minimum width). The box also covers K's centre (Newton's estimate)
// by twice the blur: centred on a sliver 1.3e-5 from a root whose K is 2e-5 wide, K poked out of it. Else what K left
// of the box is split along its wider side. Depth first; maxBoxes bounds the work (each Krawczyk test is a box).
PolyCellResult cellPolySolve(CellPoly cp, uint maxBoxes)
{
    PolyCellResult res;
    res.roots = 0u;
    res.undecided = 0u;
    res.boxes = 0u;
    res.overflow = 0u;
    for (uint k = 0u; k < kPolyMaxRoots; ++k) res.st[k] = float2(0.0);
    // One Krawczyk test a step, at one call site (each is compiled whole). A stack entry is a box of the partition (kind
    // 0: tested grown) or an inflation box (kind 1 + n: tested as it is, the n entries under it the halves of what K
    // left, dropped when it settles the box, tested when it does not).
    float4 stack[kPolyStack];
    uint kinds[kPolyStack];
    uint top = 1u;
    stack[0] = float4(0.0, 0.0, 1.0, 1.0);
    kinds[0] = 0u;
    while (top > 0u)
    {
        --top;
        const float4 box = stack[top];
        const uint kind = kinds[top];
        if (kind == 0u && polyBoxOffTriangle(cp, box)) continue;
        if (res.boxes >= maxBoxes)
        {
            ++res.undecided;
            continue;
        }
        ++res.boxes;
        const float2 g = kind == 0u ? kPolyGrow * (box.zw - box.xy) : float2(0.0);
        float2 root, blur, centre;
        float4 next;
        const int k = polyKrawczyk(cp, float4(box.xy - g, box.zw + g), root, next, blur, centre);
        if (k != 0)
        {
            if (k > 0) polyKeep(cp, root, res);
            if (kind > 0u) top -= kind - 1u;   // the halves under an inflation box that settled its box
            continue;
        }
        if (kind > 0u)
        {
            if (kind == 1u) ++res.undecided;   // nothing under it: what K left could not be split
            continue;
        }
        // Undecided: what K leaves of the box (every root of the box is in it) split along its wider side, under an
        // inflation box round what K leaves of the grown box and K's centre by twice the blur.
        const float4 rest = float4(max(next.xy, box.xy), min(next.zw, box.zw));
        const float2 rw = rest.zw - rest.xy;
        if (any(rw < 0.0)) continue;
        uint halves = 0u;
        if (max(rw.x, rw.y) >= kPolyMinWidth && top + 3u <= kPolyStack)
        {
            const bool alongS = rw.x >= rw.y;
            const float m = alongS ? rest.x + 0.5 * rw.x : rest.y + 0.5 * rw.y;
            stack[top] = alongS ? float4(rest.x, rest.y, m, rest.w) : float4(rest.x, rest.y, rest.z, m);
            kinds[top++] = 0u;
            stack[top] = alongS ? float4(m, rest.y, rest.z, rest.w) : float4(rest.x, m, rest.z, rest.w);
            kinds[top++] = 0u;
            halves = 2u;
        }
        if (top + 1u > kPolyStack)
        {
            ++res.undecided;
            continue;
        }
        const float2 lo = min(next.xy, centre - 2.0 * blur), hi = max(next.zw, centre + 2.0 * blur);
        const float2 g2 = 0.25 * (hi - lo) + kPolyMinWidth;
        stack[top] = float4(lo - g2, hi + g2);
        kinds[top++] = 1u + halves;
    }
    return res;
}
