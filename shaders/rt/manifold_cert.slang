// Certified roots for manifold connections (docs/specs/2026-11-04-caustics-step2-design.md, "Step 2b as designed"):
// the slope maps written once, generic over a number type, and the Krawczyk operator that proves where their roots
// are. A module (no entry point): rt/manifold.slang's callers and tools/caustics/manifold_kernel.slang import it.
//
// Number types (INum): MFloat (a float), MIval (an interval rounded outward: an ulp for + - *, 3 for / and sqrt,
// which Vulkan allows 2.5 and 2 ulp), MDual<T, N> (a value and its N partial derivatives, of any INum). The map run on
// MDual<MFloat, 2> gives sigma and its Jacobian at a point; on MDual<MIval, 2> it gives enclosures of both over a box.
//
// Krawczyk (Krawczyk 1969; Moore's interval Newton): on a box B with centre c, Y the inverse of J(c),
//   K(B) = c - Y F(c) + (I - Y J(B)) (B - c).
// K(B) and B disjoint: no root in B. K(B) inside B's interior: exactly one root, and Newton from c converges to it.
// Otherwise B shrinks to K(B) and is split. Path Cuts (Wang, Hasan, Yan 2020) enumerates specular paths the same way.
//
// What it took to be complete (tests/renderer/manifold_tests t9):
// - The constraint is F = (A - target.x C, B - target.y C) (H in the frame), not sigma - target: no division, so its
//   intervals stay tight where C is small; a root of F counts only where C > 0 and sigma meets the target there (F
//   also vanishes where H does).
// - Each box is tested grown by a tenth a side (epsilon inflation): a root on a split line fits inside some grown box.
//   A unique root certified in a grown box is recorded once (a root already recorded inside that box is the same one).
// - Squares are intervals' own (x.sqr() >= 0), or a length's bound reaches below 0 and every guard splits.
// - Where the interpolated normal crosses the triangle's plane (its orientation flips), each branch is solved over the
//   box and a root kept only where its orientation is its branch's.
// A box the walk cannot decide (kCertMaxBoxes spent, or narrower than kCertMinWidth) is counted in `undecided`: the
// pass reports it, never drops it silently. On the showcase's ball: 7.2 boxes a candidate triangle, none undecided.
// - Rigour at the centre: F(c) is an interval (its rounding bounded), and the mean-value form F(c) + J(B)(B - c)
//   excludes boxes the natural form cannot. Where K's width is that rounding times |J^-1| (an ill-conditioned
//   Jacobian), smaller boxes can never certify: the box is retried once grown to 8 times it (Rump's verification).

import rt.manifold;

static const float kCertEps = 1.1920929e-7;   // 2^-23: an ulp relative to the value
static const float kCertTiny = 1.0e-30;      // past underflow

interface INum
{
    static This cst(float x);
    This add(This b);
    This sub(This b);
    This mul(This b);
    This div(This b);
    This sqrtOf();
    This neg();
    // x * x, which an interval holding 0 bounds below by 0 (x.mul(x) would not: the dependency problem).
    This sqr();
    // 1: certainly > 0, -1: certainly < 0, 0: either (an interval holding 0, or a float exactly 0)
    int signOf();
    // The value as a float (an interval's midpoint).
    float mid();
}

struct MFloat : INum
{
    float v;
    static MFloat cst(float x) { MFloat r; r.v = x; return r; }
    MFloat add(MFloat b) { return MFloat.cst(v + b.v); }
    MFloat sub(MFloat b) { return MFloat.cst(v - b.v); }
    MFloat mul(MFloat b) { return MFloat.cst(v * b.v); }
    MFloat div(MFloat b) { return MFloat.cst(v / b.v); }
    MFloat sqrtOf() { return MFloat.cst(sqrt(max(v, 0.0))); }
    MFloat neg() { return MFloat.cst(-v); }
    MFloat sqr() { return MFloat.cst(v * v); }
    int signOf() { return v > 0.0 ? 1 : (v < 0.0 ? -1 : 0); }
    float mid() { return v; }
}

float certDown(float x, float ulps) { return x - (abs(x) * kCertEps * ulps + kCertTiny); }
float certUp(float x, float ulps) { return x + (abs(x) * kCertEps * ulps + kCertTiny); }

struct MIval : INum
{
    float lo, hi;
    static MIval make(float a, float b) { MIval r; r.lo = a; r.hi = b; return r; }
    static MIval cst(float x) { return MIval.make(x, x); }
    static MIval whole() { return MIval.make(-3.0e38, 3.0e38); }
    MIval add(MIval b) { return MIval.make(certDown(lo + b.lo, 1.0), certUp(hi + b.hi, 1.0)); }
    MIval sub(MIval b) { return MIval.make(certDown(lo - b.hi, 1.0), certUp(hi - b.lo, 1.0)); }
    MIval mul(MIval b)
    {
        const float p0 = lo * b.lo, p1 = lo * b.hi, p2 = hi * b.lo, p3 = hi * b.hi;
        return MIval.make(certDown(min(min(p0, p1), min(p2, p3)), 1.0), certUp(max(max(p0, p1), max(p2, p3)), 1.0));
    }
    MIval div(MIval b)
    {
        if (!(b.lo > 0.0 || b.hi < 0.0)) return whole();   // 0 in the divisor
        const float q0 = lo / b.lo, q1 = lo / b.hi, q2 = hi / b.lo, q3 = hi / b.hi;
        return MIval.make(certDown(min(min(q0, q1), min(q2, q3)), 3.0), certUp(max(max(q0, q1), max(q2, q3)), 3.0));
    }
    MIval sqrtOf() { return MIval.make(certDown(sqrt(max(lo, 0.0)), 3.0), certUp(sqrt(max(hi, 0.0)), 3.0)); }
    MIval neg() { return MIval.make(-hi, -lo); }
    MIval sqr()
    {
        const float a = lo * lo, b = hi * hi;
        const float top = certUp(max(a, b), 1.0);
        return (lo <= 0.0 && hi >= 0.0) ? MIval.make(0.0, top) : MIval.make(max(certDown(min(a, b), 1.0), 0.0), top);
    }
    int signOf() { return lo > 0.0 ? 1 : (hi < 0.0 ? -1 : 0); }
    float mid() { return 0.5 * lo + 0.5 * hi; }
    // NaN (an overflowed bound) holds zero: it proves nothing.
    bool holdsZero() { return !(lo > 0.0) && !(hi < 0.0); }
}

struct MDual<T : INum, let N : int> : INum
{
    T v;
    T d[N];
    static MDual<T, N> cst(float x)
    {
        MDual<T, N> r;
        r.v = T.cst(x);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = T.cst(0.0);
        return r;
    }
    static MDual<T, N> variable(T x, int k)
    {
        MDual<T, N> r = MDual<T, N>.cst(0.0);
        r.v = x;
        r.d[k] = T.cst(1.0);
        return r;
    }
    MDual<T, N> add(MDual<T, N> b)
    {
        MDual<T, N> r;
        r.v = v.add(b.v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].add(b.d[i]);
        return r;
    }
    MDual<T, N> sub(MDual<T, N> b)
    {
        MDual<T, N> r;
        r.v = v.sub(b.v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].sub(b.d[i]);
        return r;
    }
    MDual<T, N> mul(MDual<T, N> b)
    {
        MDual<T, N> r;
        r.v = v.mul(b.v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].mul(b.v).add(v.mul(b.d[i]));
        return r;
    }
    MDual<T, N> div(MDual<T, N> b)
    {
        // (a / b)' = (a' - (a / b) b') / b
        MDual<T, N> r;
        r.v = v.div(b.v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].sub(r.v.mul(b.d[i])).div(b.v);
        return r;
    }
    MDual<T, N> sqrtOf()
    {
        // (sqrt a)' = a' / (2 sqrt a)
        MDual<T, N> r;
        r.v = v.sqrtOf();
        const T twice = r.v.add(r.v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].div(twice);
        return r;
    }
    MDual<T, N> neg()
    {
        MDual<T, N> r;
        r.v = v.neg();
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = d[i].neg();
        return r;
    }
    MDual<T, N> sqr()
    {
        // (x^2)' = 2 x x'
        MDual<T, N> r;
        r.v = v.sqr();
        const T twice = v.add(v);
        [ForceUnroll] for (int i = 0; i < N; ++i) r.d[i] = twice.mul(d[i]);
        return r;
    }
    int signOf() { return v.signOf(); }
    float mid() { return v.mid(); }
}

// ---- vectors of any number type ---------------------------------------------------------------------
struct MV3<T : INum>
{
    T x, y, z;
    static MV3<T> of(float3 a) { MV3<T> r; r.x = T.cst(a.x); r.y = T.cst(a.y); r.z = T.cst(a.z); return r; }
    MV3<T> add(MV3<T> b) { MV3<T> r; r.x = x.add(b.x); r.y = y.add(b.y); r.z = z.add(b.z); return r; }
    MV3<T> sub(MV3<T> b) { MV3<T> r; r.x = x.sub(b.x); r.y = y.sub(b.y); r.z = z.sub(b.z); return r; }
    MV3<T> scale(T s) { MV3<T> r; r.x = x.mul(s); r.y = y.mul(s); r.z = z.mul(s); return r; }
    MV3<T> divide(T s) { MV3<T> r; r.x = x.div(s); r.y = y.div(s); r.z = z.div(s); return r; }
    T dotWith(MV3<T> b) { return x.mul(b.x).add(y.mul(b.y)).add(z.mul(b.z)); }
    T lengthSq() { return x.sqr().add(y.sqr()).add(z.sqr()); }
    MV3<T> crossWith(MV3<T> b)
    {
        MV3<T> r;
        r.x = y.mul(b.z).sub(z.mul(b.y));
        r.y = z.mul(b.x).sub(x.mul(b.z));
        r.z = x.mul(b.y).sub(y.mul(b.x));
        return r;
    }
}

// ---- 2a's map, generic -------------------------------------------------------------------------------
// The half vector's components in the frame (A = H.t, B = H.b, C = H.n; sigma = (A / C, B / C)), in any number type.
// Returns 0 when a guard cannot be decided (a sign the interval does not fix, a degenerate length), 2 when only the
// normal's orientation is undecided (the interpolated normal crosses the triangle's plane in the input: each branch is
// smooth there, and `branch` (+1, -1) forces one), 1 otherwise; C may then hold any sign (the caller decides).
int manifoldFrameT<T : INum>(ManifoldTri tri, float3 p, float3 L, T u, T v, out T A, out T B, out T C, int branch = 0)
{
    A = T.cst(0.0);
    B = T.cst(0.0);
    C = T.cst(0.0);
    const MV3<T> y = MV3<T>.of(tri.p0).add(MV3<T>.of(tri.e1).scale(u)).add(MV3<T>.of(tri.e2).scale(v));
    const MV3<T> d = MV3<T>.of(p).sub(y);
    const T rr = d.lengthSq();
    if (rr.signOf() <= 0) return 0;
    const MV3<T> w = d.divide(rr.sqrtOf());
    const MV3<T> H = w.add(MV3<T>.of(L));
    const MV3<T> N = MV3<T>.of(tri.n0).add(MV3<T>.of(tri.a1).scale(u)).add(MV3<T>.of(tri.a2).scale(v));
    const T nn = N.lengthSq();
    if (nn.signOf() <= 0) return 0;
    const MV3<T> Nh = N.divide(nn.sqrtOf());
    // The side the triangle shows p is one sign over the whole triangle: dot(g, p - y) = dot(g, p - p0).
    const float3 g = cross(tri.e1, tri.e2);
    const float side = dot(g, p - tri.p0) >= 0.0 ? 1.0 : -1.0;
    int sgn = branch;
    if (sgn == 0)
    {
        sgn = Nh.dotWith(MV3<T>.of(g * side)).signOf();
        if (sgn == 0) return 2;
    }
    const MV3<T> n = Nh.scale(T.cst(sgn < 0 ? -1.0 : 1.0));
    const MV3<T> c = MV3<T>.of(tri.c);
    const MV3<T> Tv = c.sub(n.scale(c.dotWith(n)));
    const T tt = Tv.lengthSq();
    if (tt.signOf() <= 0) return 0;
    const MV3<T> t = Tv.divide(tt.sqrtOf());
    const MV3<T> b = n.crossWith(t);
    A = H.dotWith(t);
    B = H.dotWith(b);
    C = H.dotWith(n);
    return 1;
}

// manifoldMap's sigma at (u, v), in any number type: 1 defined over the whole input, -1 certainly undefined (H
// certainly below the normal), 0 when it cannot tell.
int manifoldSigmaT<T : INum>(ManifoldTri tri, float3 p, float3 L, T u, T v, out T sx, out T sy)
{
    sx = T.cst(0.0);
    sy = T.cst(0.0);
    T A, B, C;
    if (manifoldFrameT<T>(tri, p, L, u, v, A, B, C) != 1) return 0;
    const int cs = C.signOf();
    if (cs < 0) return -1;
    if (cs == 0) return 0;
    sx = A.div(C);
    sy = B.div(C);
    return 1;
}

// The constraint the solver certifies: F = (A - target.x C, B - target.y C), whose roots with C > 0 are sigma's roots
// (F = C (sigma - target)), with no division: its intervals stay tight where C is small. -1: C certainly < 0 over
// the input (no root of sigma there); 0: undecided; 2: the orientation undecided (solve each branch); 1: F defined.
int manifoldConstraintT<T : INum>(ManifoldTri tri, float3 p, float3 L, float2 target, T u, T v, out T fx, out T fy,
                                  int branch = 0)
{
    T A, B, C;
    const int def = manifoldFrameT<T>(tri, p, L, u, v, A, B, C, branch);
    fx = A.sub(C.mul(T.cst(target.x)));
    fy = B.sub(C.mul(T.cst(target.y)));
    if (def != 1) return def;
    if (C.signOf() < 0) return -1;
    return 1;
}

// The interpolated normal's orientation at (u, v) as manifoldFrameT turns it: +1 kept, -1 flipped, 0 on the plane.
int manifoldOrientation(ManifoldTri tri, float3 p, float2 uv)
{
    const float3 g = cross(tri.e1, tri.e2);
    const float side = dot(g, p - tri.p0) >= 0.0 ? 1.0 : -1.0;
    const float d = dot(tri.n0 + tri.a1 * uv.x + tri.a2 * uv.y, g * side);
    return d > 0.0 ? 1 : (d < 0.0 ? -1 : 0);
}

// ---- Krawczyk on a triangle ---------------------------------------------------------------------------
typealias MDualF2 = MDual<MFloat, 2>;
typealias MDualI2 = MDual<MIval, 2>;

static const uint kCertMaxRoots = 4u;
static const uint kCertStack = 48u;
static const uint kCertMaxBoxes = 512u;
static const float kCertMinWidth = 1.0e-6;

struct CertResult
{
    uint roots;          // certified roots inside the triangle
    float2 uv[4];        // each one's (u, v) after Newton from its box's centre
    uint undecided;      // boxes given up on (too small, or the box budget spent): each may hold a root
    uint boxes;          // boxes examined
}

// F and its Jacobian at a point. False where undefined.
bool certAt(ManifoldTri tri, float3 p, float3 L, float2 target, float2 uv, int branch, out float2 f, out float2x2 J)
{
    MDualF2 fx, fy;
    const int ok = manifoldConstraintT<MDualF2>(tri, p, L, target, MDualF2.variable(MFloat.cst(uv.x), 0),
                                                MDualF2.variable(MFloat.cst(uv.y), 1), fx, fy, branch);
    f = float2(fx.v.v, fy.v.v);
    J = float2x2(fx.d[0].v, fx.d[1].v, fy.d[0].v, fy.d[1].v);   // rows: d F.x, d F.y
    return ok == 1 || ok == -1;
}

// Every root of sigma(u, v) = target on the triangle (u, v >= 0, u + v <= 1), certified.
CertResult manifoldCertify(ManifoldTri tri, float3 p, float3 L, float2 target)
{
    return manifoldCertifyBox(tri, p, L, target, float4(0.0, 0.0, 1.0, 1.0));
}

// The same from a box of (u, v): (u0, v0, u1, v1).
CertResult manifoldCertifyBox(ManifoldTri tri, float3 p, float3 L, float2 target, float4 start)
{
    CertResult res;
    res.roots = 0u;
    res.undecided = 0u;
    res.boxes = 0u;
    [ForceUnroll] for (uint k = 0u; k < 4u; ++k) res.uv[k] = float2(0.0);
    float4 stack[kCertStack];   // (u0, v0, u1, v1)
    int branches[kCertStack];   // 0: the orientation from the map; +1, -1: forced (a box the normal's flip crosses)
    uint top = 0u;
    branches[top] = 0;
    stack[top++] = start;
    while (top > 0u)
    {
        float4 B = stack[--top];
        const int branch = branches[top];
        if (res.boxes >= kCertMaxBoxes)
        {
            res.undecided += top + 1u;
            break;
        }
        ++res.boxes;
        if (B.x + B.y > 1.0) continue;   // wholly past the triangle's hypotenuse
        const float2 cuv = 0.5 * (B.xy + B.zw);
        // The test runs on B grown by a tenth a side (epsilon inflation): a root on a split line, or within float of
        // one, lies inside some grown box, where the plain box's K(B) could never fit in its interior.
        float2 half = 0.6 * (B.zw - B.xy) + 1e-7;
        bool decided = false, split = false;
        for (uint attempt = 0u; attempt < 2u && !decided && !split; ++attempt)
        {
            const float4 Bt = float4(cuv - half, cuv + half);
            MDualI2 fx, fy;
            const int def = manifoldConstraintT<MDualI2>(tri, p, L, target,
                                                         MDualI2.variable(MIval.make(Bt.x, Bt.z), 0),
                                                         MDualI2.variable(MIval.make(Bt.y, Bt.w), 1), fx, fy, branch);
            if (def < 0)
            {
                decided = true;   // C certainly negative over Bt: no root of sigma
                break;
            }
            if (def == 2)
            {
                // The normal's orientation flips in Bt: solve each branch over B (each is smooth across the flip),
                // keeping a root only where its own orientation is the branch's.
                decided = true;
                if (top + 2u > kCertStack)
                {
                    ++res.undecided;
                    break;
                }
                branches[top] = 1;
                stack[top++] = B;
                branches[top] = -1;
                stack[top++] = B;
                break;
            }
            if (def == 0 || !fx.v.holdsZero() || !fy.v.holdsZero())
            {
                decided = def != 0;   // F(Bt) excludes 0; an undecidable guard splits
                split = def == 0;
                break;
            }
            // F at the centre as an interval (its own rounding bounded), then the mean-value form
            // F(Bt) in F(c) + J(Bt) (Bt - c): its excess shrinks with the box's square, the natural one's with its width.
            MIval fcx, fcy;
            const int defc = manifoldConstraintT<MIval>(tri, p, L, target, MIval.cst(cuv.x), MIval.cst(cuv.y), fcx,
                                                        fcy, branch);
            float2 fc;
            float2x2 Jc;
            if (!(defc == 1 || defc == -1) || !certAt(tri, p, L, target, cuv, branch, fc, Jc))
            {
                split = true;
                break;
            }
            const MIval du = MIval.make(Bt.x - cuv.x, Bt.z - cuv.x);
            const MIval dv = MIval.make(Bt.y - cuv.y, Bt.w - cuv.y);
            const MIval mx = fcx.add(fx.d[0].mul(du)).add(fx.d[1].mul(dv));
            const MIval my = fcy.add(fy.d[0].mul(du)).add(fy.d[1].mul(dv));
            if (!mx.holdsZero() || !my.holdsZero())
            {
                decided = true;
                break;
            }
            const float det = Jc[0][0] * Jc[1][1] - Jc[0][1] * Jc[1][0];
            if (!(abs(det) > 1e-30))
            {
                split = true;
                break;
            }
            const float2x2 Y = float2x2(Jc[1][1], -Jc[0][1], -Jc[1][0], Jc[0][0]) / det;
            // M = I - Y J(Bt), intervals; J(Bt) rows fx.d, fy.d.
            MIval M[2][2];
            [ForceUnroll] for (int i = 0; i < 2; ++i)
            {
                [ForceUnroll] for (int j = 0; j < 2; ++j)
                {
                    const MIval yj = MIval.cst(Y[i][0]).mul(fx.d[j]).add(MIval.cst(Y[i][1]).mul(fy.d[j]));
                    M[i][j] = MIval.cst(i == j ? 1.0 : 0.0).sub(yj);
                }
            }
            // Y F(c) with F(c) the interval above, so K(Bt) bounds every root rigorously.
            const MIval yfx = MIval.cst(Y[0][0]).mul(fcx).add(MIval.cst(Y[0][1]).mul(fcy));
            const MIval yfy = MIval.cst(Y[1][0]).mul(fcx).add(MIval.cst(Y[1][1]).mul(fcy));
            const MIval Ku = MIval.cst(cuv.x).sub(yfx).add(M[0][0].mul(du)).add(M[0][1].mul(dv));
            const MIval Kv = MIval.cst(cuv.y).sub(yfy).add(M[1][0].mul(du)).add(M[1][1].mul(dv));
            if (Ku.hi < Bt.x || Ku.lo > Bt.z || Kv.hi < Bt.y || Kv.lo > Bt.w)
            {
                decided = true;   // no root in Bt, so none in B
                break;
            }
            if (Ku.lo > Bt.x && Ku.hi < Bt.z && Kv.lo > Bt.y && Kv.hi < Bt.w)
            {
                decided = true;   // exactly one root of F in Bt (it may lie in a neighbour's part of Bt)
                float2 uv = cuv;
                for (uint it = 0u; it < 24u; ++it)
                {
                    float2 f;
                    float2x2 J;
                    if (!certAt(tri, p, L, target, uv, branch, f, J)) break;
                    const float dj = J[0][0] * J[1][1] - J[0][1] * J[1][0];
                    if (!(abs(dj) > 1e-30)) break;
                    const float2 step = mul(float2x2(J[1][1], -J[0][1], -J[1][0], J[0][0]) / dj, f);
                    uv = clamp(uv - step, Bt.xy, Bt.zw);
                    if (all(abs(step) <= 1e-7 * max(1.0, max(abs(uv.x), abs(uv.y))))) break;
                }
                // A root of F is sigma's where H is above the normal (F's roots with C <= 0 are not), sigma itself
                // meets the target there (F also vanishes where H does: the direction to p opposite the light), and,
                // on a forced branch, the normal's own orientation is the branch's.
                MFloat A, Bc, C;
                const bool framed = manifoldFrameT<MFloat>(tri, p, L, MFloat.cst(uv.x), MFloat.cst(uv.y), A, Bc, C,
                                                           branch) == 1;
                const float2 sigmaAt = framed && C.v > 0.0 ? float2(A.v, Bc.v) / C.v : float2(3.0e38);
                const bool above = framed && C.v > 0.0 &&
                                   all(abs(sigmaAt - target) <= 1e-3 * max(1.0, length(target))) &&
                                   (branch == 0 || manifoldOrientation(tri, p, uv) == branch);
                // Bt holds exactly one root: one already recorded inside Bt is this one (a neighbour's grown box
                // certified it first).
                bool seen = false;
                for (uint q = 0u; q < res.roots; ++q)
                    seen = seen || all(res.uv[q] >= Bt.xy && res.uv[q] <= Bt.zw) || all(abs(res.uv[q] - uv) <= 1e-6);
                if (above && !seen && uv.x >= 0.0 && uv.y >= 0.0 && uv.x + uv.y <= 1.0)
                {
                    if (res.roots < kCertMaxRoots)
                    {
                        res.uv[res.roots] = uv;
                        ++res.roots;
                    }
                    else
                    {
                        ++res.undecided;
                    }
                }
                break;
            }
            // Neither: when K(Bt)'s width is the centre's rounding (|Y| times F(c)'s width), no smaller box can
            // certify; retry once on Bt grown to eight times that (Rump's verification), else shrink and split.
            const float noise = max(yfx.hi - yfx.lo, yfy.hi - yfy.lo);
            if (attempt == 0u && 4.0 * noise > min(half.x, half.y))
            {
                half = max(half, float2(8.0 * noise));
                continue;
            }
            // Shrink B to K(Bt) (Krawczyk: every root in Bt, so in B, lies in K(Bt)).
            B = float4(max(B.x, Ku.lo), max(B.y, Kv.lo), min(B.z, Ku.hi), min(B.w, Kv.hi));
            // K(Bt) misses B (it meets only the growth): no root in B.
            if (B.x > B.z || B.y > B.w) decided = true;
            else split = true;
        }
        if (decided) continue;
        // Split along the wider side.
        const float wu = B.z - B.x, wv = B.w - B.y;
        if (max(wu, wv) < kCertMinWidth || top + 2u > kCertStack)
        {
            ++res.undecided;
            continue;
        }
        if (wu >= wv)
        {
            const float m = 0.5 * (B.x + B.z);
            branches[top] = branch;
            stack[top++] = float4(B.x, B.y, m, B.w);
            branches[top] = branch;
            stack[top++] = float4(m, B.y, B.z, B.w);
        }
        else
        {
            const float m = 0.5 * (B.y + B.w);
            branches[top] = branch;
            stack[top++] = float4(B.x, B.y, B.z, m);
            branches[top] = branch;
            stack[top++] = float4(B.x, m, B.z, B.w);
        }
    }
    return res;
}
