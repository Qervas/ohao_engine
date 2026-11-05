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

// ---- step 2b: the sun through a solid, two refractions -------------------------------------------------
// Unknowns (u1, v1) on the entry triangle t1 and (u2, v2) on the exit triangle t2, of one solid of relative index eta
// (inside over outside), its vertex normals outward. The light comes from the sun (L toward it), refracts into the
// solid at y1, crosses to y2 and refracts out toward X. Each refraction's generalised half vector (Walter et al. 2007,
// h = -(eta_i i + eta_o o), i and o pointing away from the surface on their own sides) is drawn, as a slope in its
// triangle's frame (2a's: n the interpolated normal, t from the cluster's c, b = n x t), so the constraint is two of
// 2a's, one a vertex:
//   at y1: h1 = -(L + eta d),        d = normalize(y2 - y1)  (i = L outside, o = d inside)
//   at y2: h2 = eta d - w,           w = normalize(X - y2)   (i = -d inside, o = w outside)
// both outward (along n) at normal incidence, as (eta - 1) n. F = (A1 - s1.x C1, B1 - s1.y C1, A2 - s2.x C2,
// B2 - s2.y C2), Ak = hk.tk, Bk = hk.bk, Ck = hk.nk: no division, roots with Ck > 0 are the slopes' roots.

struct ManifoldPairResult
{
    uint roots;          // certified paths
    float4 uv[4];        // each one's (u1, v1, u2, v2)
    uint undecided;      // boxes given up on: each may hold a path (counted, never dropped silently)
    uint boxes;          // boxes examined
}

static const uint kPairMaxBoxes = 2048u;
static const uint kPairStack = 64u;
static const float kPairMinWidth = 1.0e-6;

// The frame of a triangle at (u, v): its interpolated normal as authored (a solid's are outward), t from its c.
// 0 when a length cannot be bounded away from 0.
int manifoldFrameAtT<T : INum>(ManifoldTri tri, T u, T v, out MV3<T> n, out MV3<T> t, out MV3<T> b)
{
    const MV3<T> N = MV3<T>.of(tri.n0).add(MV3<T>.of(tri.a1).scale(u)).add(MV3<T>.of(tri.a2).scale(v));
    const T nn = N.lengthSq();
    n = N;
    t = N;
    b = N;
    if (nn.signOf() <= 0) return 0;
    n = N.divide(nn.sqrtOf());
    const MV3<T> c = MV3<T>.of(tri.c);
    const MV3<T> Tv = c.sub(n.scale(c.dotWith(n)));
    const T tt = Tv.lengthSq();
    if (tt.signOf() <= 0) return 0;
    t = Tv.divide(tt.sqrtOf());
    b = n.crossWith(t);
    return 1;
}

// F at (u1, v1, u2, v2) in any number type; c1, c2 receive C1, C2. 0 when undecidable, 1 otherwise.
int manifoldPairT<T : INum>(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, T u1, T v1,
                            T u2, T v2, out T f0, out T f1, out T f2, out T f3, out T c1, out T c2)
{
    f0 = T.cst(0.0);
    f1 = f0;
    f2 = f0;
    f3 = f0;
    c1 = f0;
    c2 = f0;
    const MV3<T> y1 = MV3<T>.of(t1.p0).add(MV3<T>.of(t1.e1).scale(u1)).add(MV3<T>.of(t1.e2).scale(v1));
    const MV3<T> y2 = MV3<T>.of(t2.p0).add(MV3<T>.of(t2.e1).scale(u2)).add(MV3<T>.of(t2.e2).scale(v2));
    const MV3<T> D = y2.sub(y1);
    const T dd = D.lengthSq();
    if (dd.signOf() <= 0) return 0;
    const MV3<T> d = D.divide(dd.sqrtOf());
    const MV3<T> W = MV3<T>.of(X).sub(y2);
    const T ww = W.lengthSq();
    if (ww.signOf() <= 0) return 0;
    const MV3<T> w = W.divide(ww.sqrtOf());
    const MV3<T> h1 = MV3<T>.of(-L).sub(d.scale(T.cst(eta)));
    const MV3<T> h2 = d.scale(T.cst(eta)).sub(w);
    MV3<T> n1, ta, ba, n2, tb, bb;
    if (manifoldFrameAtT<T>(t1, u1, v1, n1, ta, ba) == 0) return 0;
    if (manifoldFrameAtT<T>(t2, u2, v2, n2, tb, bb) == 0) return 0;
    c1 = h1.dotWith(n1);
    c2 = h2.dotWith(n2);
    f0 = h1.dotWith(ta).sub(c1.mul(T.cst(target.x)));
    f1 = h1.dotWith(ba).sub(c1.mul(T.cst(target.y)));
    f2 = h2.dotWith(tb).sub(c2.mul(T.cst(target.z)));
    f3 = h2.dotWith(bb).sub(c2.mul(T.cst(target.w)));
    return 1;
}

typealias MDualF4 = MDual<MFloat, 4>;
typealias MDualI4 = MDual<MIval, 4>;

// F and its Jacobian at a point (rows: F's components, columns: u1, v1, u2, v2), and C1, C2 (each vertex's h . n).
// False where undefined.
bool pairAtC(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, float4 x, out float4 f,
             out float4x4 J, out float2 c)
{
    MDualF4 f0, f1, f2, f3, c1, c2;
    const int ok = manifoldPairT<MDualF4>(t1, t2, X, L, eta, target, MDualF4.variable(MFloat.cst(x.x), 0),
                                          MDualF4.variable(MFloat.cst(x.y), 1), MDualF4.variable(MFloat.cst(x.z), 2),
                                          MDualF4.variable(MFloat.cst(x.w), 3), f0, f1, f2, f3, c1, c2);
    f = float4(f0.v.v, f1.v.v, f2.v.v, f3.v.v);
    J = float4x4(f0.d[0].v, f0.d[1].v, f0.d[2].v, f0.d[3].v, f1.d[0].v, f1.d[1].v, f1.d[2].v, f1.d[3].v,
                 f2.d[0].v, f2.d[1].v, f2.d[2].v, f2.d[3].v, f3.d[0].v, f3.d[1].v, f3.d[2].v, f3.d[3].v);
    c = float2(c1.v.v, c2.v.v);
    return ok == 1;
}

bool pairAt(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, float4 x, out float4 f,
            out float4x4 J)
{
    float2 c;
    return pairAtC(t1, t2, X, L, eta, target, x, f, J, c);
}

// The inverse of a 4 x 4 matrix by Gauss-Jordan with partial pivoting; false when a pivot is under 1e-30.
bool invert4(float4x4 A, out float4x4 inv)
{
    float m[4][8];
    [ForceUnroll] for (int i = 0; i < 4; ++i)
    {
        [ForceUnroll] for (int j = 0; j < 4; ++j)
        {
            m[i][j] = A[i][j];
            m[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    inv = float4x4(0.0);
    for (int col = 0; col < 4; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (abs(m[r][col]) > abs(m[piv][col])) piv = r;
        if (!(abs(m[piv][col]) > 1e-30)) return false;
        for (int k = 0; k < 8; ++k)
        {
            const float tmp = m[col][k];
            m[col][k] = m[piv][k];
            m[piv][k] = tmp;
        }
        const float s = 1.0 / m[col][col];
        for (int k = 0; k < 8; ++k) m[col][k] *= s;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col) continue;
            const float fct = m[r][col];
            for (int k = 0; k < 8; ++k) m[r][k] -= fct * m[col][k];
        }
    }
    [ForceUnroll] for (int i = 0; i < 4; ++i)
    {
        [ForceUnroll] for (int j = 0; j < 4; ++j) inv[i][j] = m[i][j + 4];
    }
    return true;
}

// A certified path's checks in float: both refractions physical (light on the right side of each microfacet normal,
// h1 and h2, which F's roots make the drawn ones) and the slopes met.
bool pairPhysical(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, float4 x)
{
    const float3 y1 = t1.p0 + t1.e1 * x.x + t1.e2 * x.y;
    const float3 y2 = t2.p0 + t2.e1 * x.z + t2.e2 * x.w;
    const float3 d = normalize(y2 - y1), w = normalize(X - y2);
    const float3 h1 = -(L + eta * d), h2 = eta * d - w;
    MFloat A1, B1, C1, A2, B2, C2;
    MV3<MFloat> n1, ta, ba, n2, tb, bb;
    if (manifoldFrameAtT<MFloat>(t1, MFloat.cst(x.x), MFloat.cst(x.y), n1, ta, ba) == 0) return false;
    if (manifoldFrameAtT<MFloat>(t2, MFloat.cst(x.z), MFloat.cst(x.w), n2, tb, bb) == 0) return false;
    const float3 nn1 = float3(n1.x.v, n1.y.v, n1.z.v), nn2 = float3(n2.x.v, n2.y.v, n2.z.v);
    const float3 tt1 = float3(ta.x.v, ta.y.v, ta.z.v), bt1 = float3(ba.x.v, ba.y.v, ba.z.v);
    const float3 tt2 = float3(tb.x.v, tb.y.v, tb.z.v), bt2 = float3(bb.x.v, bb.y.v, bb.z.v);
    const float k1 = dot(h1, nn1), k2 = dot(h2, nn2);
    if (!(k1 > 0.0) || !(k2 > 0.0)) return false;
    const float2 s1 = float2(dot(h1, tt1), dot(h1, bt1)) / k1, s2 = float2(dot(h2, tt2), dot(h2, bt2)) / k2;
    const float tol = 1e-3;
    if (any(abs(s1 - target.xy) > tol * max(1.0, length(target.xy))) ||
        any(abs(s2 - target.zw) > tol * max(1.0, length(target.zw))))
        return false;
    // Light outside at y1 and inside after it; inside before y2 and outside after it (each against its microfacet
    // normal, the half vector).
    return dot(L, h1) > 0.0 && dot(d, h1) < 0.0 && dot(d, h2) > 0.0 && dot(w, h2) > 0.0;
}

// One Krawczyk test of the pair map on the box c +- half (c need not be the box's centre of any split: a root found by
// Newton may anchor it). Returns -1 when the box certainly holds no path (C certainly negative, F certainly missing 0
// by the natural or the mean-value form, or K disjoint), 1 when it holds exactly one root of F (K inside), 2 when a
// guard cannot be decided, 0 otherwise; K and the width of Y F(c) (the centre's rounding through Y) come back.
int pairKrawczyk(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, float4 c, float4 half,
                 out MIval K[4], out float noise)
{
    [ForceUnroll] for (int i = 0; i < 4; ++i) K[i] = MIval.cst(0.0);
    noise = 0.0;
    const float4 bl = c - half, bh = c + half;
    MDualI4 g0, g1, g2, g3, gc1, gc2;
    const int def = manifoldPairT<MDualI4>(t1, t2, X, L, eta, target, MDualI4.variable(MIval.make(bl.x, bh.x), 0),
                                           MDualI4.variable(MIval.make(bl.y, bh.y), 1),
                                           MDualI4.variable(MIval.make(bl.z, bh.z), 2),
                                           MDualI4.variable(MIval.make(bl.w, bh.w), 3), g0, g1, g2, g3, gc1, gc2);
    if (def == 0) return 2;
    if (gc1.v.signOf() < 0 || gc2.v.signOf() < 0 || !g0.v.holdsZero() || !g1.v.holdsZero() || !g2.v.holdsZero() ||
        !g3.v.holdsZero())
        return -1;
    MIval fc0, fc1, fc2, fc3, cc1, cc2;
    const int defc = manifoldPairT<MIval>(t1, t2, X, L, eta, target, MIval.cst(c.x), MIval.cst(c.y), MIval.cst(c.z),
                                          MIval.cst(c.w), fc0, fc1, fc2, fc3, cc1, cc2);
    float4 fcf;
    float4x4 Jc, Y;
    if (defc == 0 || !pairAt(t1, t2, X, L, eta, target, c, fcf, Jc) || !invert4(Jc, Y)) return 2;
    MIval FC[4] = { fc0, fc1, fc2, fc3 };
    MIval dx[4] = { MIval.make(bl.x - c.x, bh.x - c.x), MIval.make(bl.y - c.y, bh.y - c.y),
                    MIval.make(bl.z - c.z, bh.z - c.z), MIval.make(bl.w - c.w, bh.w - c.w) };
    MIval JB[4][4];
    [ForceUnroll] for (int j = 0; j < 4; ++j)
    {
        JB[0][j] = g0.d[j];
        JB[1][j] = g1.d[j];
        JB[2][j] = g2.d[j];
        JB[3][j] = g3.d[j];
    }
    // The mean-value form: F(B) in F(c) + J(B) (B - c).
    [ForceUnroll] for (int i = 0; i < 4; ++i)
    {
        MIval s = FC[i];
        [ForceUnroll] for (int j = 0; j < 4; ++j) s = s.add(JB[i][j].mul(dx[j]));
        if (!s.holdsZero()) return -1;
    }
    // K = c - Y F(c) + (I - Y J(B)) (B - c).
    [ForceUnroll] for (int i = 0; i < 4; ++i)
    {
        MIval yf = MIval.cst(0.0);
        [ForceUnroll] for (int k = 0; k < 4; ++k) yf = yf.add(MIval.cst(Y[i][k]).mul(FC[k]));
        noise = max(noise, yf.hi - yf.lo);
        MIval acc = MIval.cst(c[i]).sub(yf);
        [ForceUnroll] for (int j = 0; j < 4; ++j)
        {
            MIval yj = MIval.cst(0.0);
            [ForceUnroll] for (int k = 0; k < 4; ++k) yj = yj.add(MIval.cst(Y[i][k]).mul(JB[k][j]));
            acc = acc.add(MIval.cst(i == j ? 1.0 : 0.0).sub(yj).mul(dx[j]));
        }
        K[i] = acc;
    }
    bool inside = true;
    [ForceUnroll] for (int i = 0; i < 4; ++i)
    {
        if (K[i].hi < bl[i] || K[i].lo > bh[i]) return -1;
        inside = inside && K[i].lo > bl[i] && K[i].hi < bh[i];
    }
    return inside ? 1 : 0;
}

// Newton on the pair map from x within [lo, hi]; false unless it converges there.
bool pairNewton(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target, float4 lo, float4 hi,
                inout float4 x)
{
    for (uint it = 0u; it < 24u; ++it)
    {
        float4 f;
        float4x4 J, Ji;
        if (!pairAt(t1, t2, X, L, eta, target, x, f, J) || !invert4(J, Ji)) return false;
        const float4 step = mul(Ji, f);
        x -= step;
        if (any(x < lo) || any(x > hi)) return false;
        if (all(abs(step) <= 1e-6)) return true;   // a few ulps at these coordinates: float cannot settle finer
    }
    return false;
}

// Every path through (t1, t2) whose slopes are target.xy at y1 and target.zw at y2, certified: the walk of
// manifoldCertifyBox in four dimensions (epsilon inflation, F(c) an interval, the mean-value exclusion). Where K is
// neither inside nor disjoint, Newton from the box's centre anchors a second test on a box centred at the root it finds
// and covering the grown box (Rump's verification): there F(c) is all but 0, so the centre's rounding no longer swamps
// K, which at a split's centre kept boxes round a root from ever certifying (two of 120 built paths, 2026-11-05).
ManifoldPairResult manifoldCertifyPair(ManifoldTri t1, ManifoldTri t2, float3 X, float3 L, float eta, float4 target)
{
    ManifoldPairResult res;
    res.roots = 0u;
    res.undecided = 0u;
    res.boxes = 0u;
    [ForceUnroll] for (uint k = 0u; k < 4u; ++k) res.uv[k] = float4(0.0);
    float4 lows[kPairStack], highs[kPairStack];
    uint top = 0u;
    lows[top] = float4(0.0);
    highs[top++] = float4(1.0);
    while (top > 0u)
    {
        --top;
        float4 lo = lows[top], hi = highs[top];
        if (res.boxes >= kPairMaxBoxes)
        {
            res.undecided += top + 1u;
            break;
        }
        ++res.boxes;
        if (lo.x + lo.y > 1.0 || lo.z + lo.w > 1.0) continue;   // past a triangle's hypotenuse
        const float4 cx = 0.5 * (lo + hi);
        const float4 half = 0.6 * (hi - lo) + 1e-7;   // the box grown by a tenth a side
        MIval K[4];
        float noise;
        int verdict = pairKrawczyk(t1, t2, X, L, eta, target, cx, half, K, noise);
        float4 root = cx, testedC = cx, testedHalf = half;
        if (verdict == 0 && 4.0 * noise > min(min(half.x, half.y), min(half.z, half.w)))
        {
            // The centre's rounding swamps K: retry once on the box grown to 8 times it.
            const float4 grown = max(half, float4(8.0 * noise));
            MIval K1[4];
            float noise1;
            const int again = pairKrawczyk(t1, t2, X, L, eta, target, cx, grown, K1, noise1);
            if (again == 1 || again == -1)
            {
                verdict = again;
                testedHalf = grown;
            }
            else if (again == 0)
            {
                // Shrink below by the grown box's K, which the centre's rounding does not swamp.
                [ForceUnroll] for (int i = 0; i < 4; ++i) K[i] = K1[i];
            }
        }
        if (verdict == 0)
        {
            // Anchor at the root Newton finds near the box, on a box covering the grown one.
            float4 x = cx;
            if (pairNewton(t1, t2, X, L, eta, target, cx - 4.0 * half, cx + 4.0 * half, x))
            {
                MIval K2[4];
                float noise2;
                float4 cover = abs(x - cx) + half * 1.05 + 1e-7;
                int anchored = pairKrawczyk(t1, t2, X, L, eta, target, x, cover, K2, noise2);
                if (anchored == 0 && 4.0 * noise2 > min(min(cover.x, cover.y), min(cover.z, cover.w)))
                {
                    // Even at the root F's rounding through Y can outgrow a tiny box: test one 8 times that wide.
                    cover = max(cover, float4(8.0 * noise2));
                    anchored = pairKrawczyk(t1, t2, X, L, eta, target, x, cover, K2, noise2);
                }
                if (anchored != 0 && anchored != 2)
                {
                    verdict = anchored;
                    root = x;
                    testedC = x;
                    testedHalf = cover;
                }
            }
        }
        if (verdict == -1) continue;   // no path in the grown box, so none in this one
        if (verdict == 1)
        {
            // Exactly one root of F in the box tested (it may lie in a neighbour's part of it).
            float4 x = root;
            pairNewton(t1, t2, X, L, eta, target, float4(-1.0), float4(2.0), x);
            bool seen = false;
            for (uint q = 0u; q < res.roots; ++q)
                seen = seen || all(abs(res.uv[q] - testedC) <= testedHalf) || all(abs(res.uv[q] - x) <= 1e-6);
            const bool inTris = all(x >= 0.0) && x.x + x.y <= 1.0 && x.z + x.w <= 1.0;
            if (!seen && inTris && pairPhysical(t1, t2, X, L, eta, target, x))
            {
                if (res.roots < 4u)
                {
                    res.uv[res.roots] = x;
                    ++res.roots;
                }
                else
                {
                    ++res.undecided;
                }
            }
            continue;
        }
        if (verdict == 0)
        {
            // Shrink to K (every root in the grown box lies in it), then split.
            [ForceUnroll] for (int i = 0; i < 4; ++i)
            {
                lo[i] = max(lo[i], K[i].lo);
                hi[i] = min(hi[i], K[i].hi);
            }
            if (any(lo > hi)) continue;
        }
        // Split along the widest side.
        const float4 wd = hi - lo;
        int axis = 0;
        [ForceUnroll] for (int i = 1; i < 4; ++i)
            if (wd[i] > wd[axis]) axis = i;
        if (wd[axis] < kPairMinWidth || top + 2u > kPairStack)
        {
            ++res.undecided;
            continue;
        }
        const float m = 0.5 * (lo[axis] + hi[axis]);
        float4 hiA = hi, loB = lo;
        hiA[axis] = m;
        loB[axis] = m;
        lows[top] = lo;
        highs[top++] = hiA;
        lows[top] = loB;
        highs[top++] = hi;
    }
    return res;
}

// ---- step 2b: the seeded walk (ruling M12) --------------------------------------------------------------
// What the walk needs of the scene: the nearest triangle a ray meets (its index, barycentric (u, v) and distance) and a
// triangle by index. The GPU pass answers with ray queries; the CPU twin with a loop over a buffer.
interface IManifoldScene
{
    bool trace(float3 o, float3 d, float tMax, out uint tri, out float2 uv, out float t);
    ManifoldTri triangle(uint i);
}

struct ManifoldWalkResult
{
    bool found;          // a path reached and certified
    uint entry, exit;    // its triangles
    float4 x;            // (u1, v1, u2, v2)
    uint steps;          // Newton steps taken
    uint roots;          // the certified roots of the final pair (the found one among them)
    uint undecided;      // boxes the certification left undecided
    // |det d(s1, s2) / d(u1, v1, u2, v2)| at the path: the slopes' Jacobian over the two triangles' parameters. At a root
    // F = (h . t - s C) is 0, so dF/dx = C ds/dx row by row (C = h . n at that vertex): det(ds/dx) = det(dF/dx) / (C1 C2)^2.
    float slopeDet;
}

static const uint kWalkSteps = 32u;
static const float kWalkTol = 1e-5;

float3 manifoldPoint(ManifoldTri t, float2 uv) { return t.p0 + t.e1 * uv.x + t.e2 * uv.y; }

// The path X <- exit <- entry <- sun through a solid of relative index eta with slopes target.xy (entry) and target.zw
// (exit), walked from a seed: the line from X along seedDir meets the exit first, then the entry (the shadow ray's,
// seedDir = L, is MNEE's). CERTIFY: the final pair's roots certified (the CPU twin's t14); without it a path is where
// the walk converges (Zeltner et al. 2020's estimate needs no more, and the 4-D interval walk is 3.5 MB of SPIR-V).
ManifoldWalkResult manifoldWalkPair<S : IManifoldScene, let CERTIFY : bool>(S scene, float3 X, float3 L, float eta,
                                                                          float4 target, float3 seedDir)
{
    ManifoldWalkResult res;
    res.found = false;
    res.entry = 0u;
    res.exit = 0u;
    res.x = float4(0.0);
    res.steps = 0u;
    res.roots = 0u;
    res.undecided = 0u;
    res.slopeDet = 0.0;
    // The seed.
    uint t2, t1;
    float2 uv2, uv1;
    float th;
    if (!scene.trace(X, seedDir, 1e30, t2, uv2, th)) return res;
    const float3 y2s = manifoldPoint(scene.triangle(t2), uv2);
    if (!scene.trace(y2s + seedDir * 1e-4, seedDir, 1e30, t1, uv1, th)) return res;
    float4 x = float4(uv1, uv2);
    ManifoldTri A = scene.triangle(t1), B = scene.triangle(t2);
    float4 f;
    float4x4 J;
    float2 cc;
    if (!pairAtC(A, B, X, L, eta, target, x, f, J, cc)) return res;
    float err = max(max(abs(f.x), abs(f.y)), max(abs(f.z), abs(f.w)));
    for (uint it = 0u; it < kWalkSteps && err > kWalkTol; ++it)
    {
        float4x4 Ji;
        if (!invert4(J, Ji)) return res;
        const float4 delta = -mul(Ji, f);
        float beta = 1.0;
        bool moved = false;
        for (uint halving = 0u; halving < 8u && !moved; ++halving, beta *= 0.5)
        {
            const float4 xs = x + beta * delta;
            // Re-project onto the mesh: X toward the stepped exit point, then from there toward the stepped entry point.
            const float3 p2 = manifoldPoint(B, xs.zw), p1 = manifoldPoint(A, xs.xy);
            uint n2, n1;
            float2 q2, q1;
            float tt;
            if (!scene.trace(X, normalize(p2 - X), 1e30, n2, q2, tt)) continue;
            const float3 y2 = manifoldPoint(scene.triangle(n2), q2);
            const float3 toward = p1 - y2;
            if (!(dot(toward, toward) > 0.0)) continue;
            const float3 dir = normalize(toward);
            if (!scene.trace(y2 + dir * 1e-4, dir, 1e30, n1, q1, tt)) continue;
            const ManifoldTri A2 = scene.triangle(n1), B2 = scene.triangle(n2);
            const float4 x2 = float4(q1, q2);
            float4 f2;
            float4x4 J2;
            float2 c2;
            if (!pairAtC(A2, B2, X, L, eta, target, x2, f2, J2, c2)) continue;
            const float err2 = max(max(abs(f2.x), abs(f2.y)), max(abs(f2.z), abs(f2.w)));
            if (!(err2 < err)) continue;
            x = x2;
            t1 = n1;
            t2 = n2;
            A = A2;
            B = B2;
            f = f2;
            J = J2;
            cc = c2;
            err = err2;
            moved = true;
        }
        ++res.steps;
        if (!moved) return res;   // no step reduced the error: no path from this seed
    }
    if (!(err <= kWalkTol)) return res;
    res.entry = t1;
    res.exit = t2;
    if (!CERTIFY)
    {
        // The walk stops where |F| < kWalkTol, and where the slopes barely move with x (a caustic's focus: det dF/dx
        // near 0) that region straddles the pair's edges: a root of the neighbouring pair, stopped at on this pair's
        // side, counted in both (t15's GPU form 10% bright on the axis). So the root is polished in this pair's own
        // parameters, extended past its edges (Newton without re-projection), and kept only inside both triangles:
        // every root belongs to one pair.
        for (uint it = 0u; it < 2u; ++it)
        {
            float4x4 Ji;
            if (!invert4(J, Ji)) return res;
            x -= mul(Ji, f);
            if (!pairAtC(A, B, X, L, eta, target, x, f, J, cc)) return res;
        }
        const float c12 = cc.x * cc.x * cc.y * cc.y;
        res.slopeDet = c12 > 0.0 ? abs(determinant(J)) / c12 : 0.0;
        // A root of F is a path only where it refracts as light does (pairPhysical: each half vector on its normal's
        // side, the light outside then inside at y1, inside then outside at y2): without that check the walk took
        // roots no light follows (t15's GPU form read 7-20x bright).
        res.found = all(x >= 0.0) && x.x + x.y <= 1.0 && x.z + x.w <= 1.0 && pairPhysical(A, B, X, L, eta, target, x);
        res.x = x;
        return res;
    }
    const float c12 = cc.x * cc.x * cc.y * cc.y;
    res.slopeDet = c12 > 0.0 ? abs(determinant(J)) / c12 : 0.0;
    // Certify the final pair: every root in it, the walked one among them.
    const ManifoldPairResult c = manifoldCertifyPair(A, B, X, L, eta, target);
    res.roots = c.roots;
    res.undecided = c.undecided;
    for (uint q = 0u; q < c.roots; ++q)
    {
        if (all(abs(c.uv[q] - x) <= 1e-3))
        {
            res.found = true;
            res.x = c.uv[q];
        }
    }
    return res;
}
