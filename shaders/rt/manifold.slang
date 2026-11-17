// Manifold connections (sub-project 24 step 2, docs/specs/2026-11-04-caustics-step2-design.md, "Step 2a as
// designed"): the pieces of the estimator that solve for the point y on a smooth triangle where the sun's light
// reflects toward a rough point p. Pure functions: rt/path_tracer.slang's manifold rounds call them, and
// tools/caustics/manifold_kernel.slang runs them on the CPU for tests/renderer/manifold_tests.
//
// The map. On a triangle y(u, v) = p0 + u e1 + v e2, its interpolated shading normal N(u, v) = n0 + u a1 + v a2
// (world normals by the inverse transpose, unnormalised: fillHit's normalize(mul(nObj, w2o)) is the same direction),
// turned to the side the triangle shows p. With w = normalize(p - y) and L toward the light, the half vector is
// H = w + L, and sigma(u, v) is its slope in the frame n, t = normalize(c - (c.n) n), b = n x t, with c a unit vector
// of the triangle's cluster (perpendicular to its cone's axis): sigma = (H.t / H.n, H.b / H.n), so H is along
// n + sigma.x t + sigma.y b. Any smooth frame is a valid change of variables on one triangle, so the choice is exact;
// one c for a whole cluster makes a slope point the same way across it, which is what lets manifoldDiscTest prune to
// the triangles near one point instead of a ring (|sigma| = tan(angle(H, n)) alone allows a ring round a sphere).
//
// The estimator draws sigma (manifoldDrawSlope), solves sigma(u, v) = sigma* on every candidate triangle
// (manifoldSolve), and weighs each root by 1 / (p(sigma) |det d sigma / d(u, v)| / |e1 x e2|) (area per slope).

static const float kManifoldPi = 3.14159265358979;

struct ManifoldTri
{
    float3 p0, e1, e2;   // world corners: p0, p1 - p0, p2 - p0
    float3 n0, a1, a2;   // world vertex normals (unnormalised): n0, n1 - n0, n2 - n0
    float3 c;            // the frame's reference: its cluster's, unit, perpendicular to the cluster's normal cone
};

// A triangle in world space from its object-space corners and vertex normals and its instance's world matrix (columns
// m0..m3, as InstanceGpu stores them): normals by the inverse transpose, unnormalised (fillHit's direction). c is 0.
ManifoldTri manifoldTriangleFrom(float4 m0, float4 m1, float4 m2, float4 m3, float3 p0, float3 p1, float3 p2, float3 o0,
                                 float3 o1, float3 o2)
{
    ManifoldTri tri;
    const float3 w0 = (m0 * p0.x + m1 * p0.y + m2 * p0.z + m3).xyz;
    const float3 w1 = (m0 * p1.x + m1 * p1.y + m2 * p1.z + m3).xyz;
    const float3 w2 = (m0 * p2.x + m1 * p2.y + m2 * p2.z + m3).xyz;
    // The inverse transpose: columns (c1 x c2, c2 x c0, c0 x c1) / det.
    const float3 x = m0.xyz, y = m1.xyz, z = m2.xyz;
    const float det = dot(x, cross(y, z));
    const float3 ix = cross(y, z) / det, iy = cross(z, x) / det, iz = cross(x, y) / det;
    const float3 n0 = ix * o0.x + iy * o0.y + iz * o0.z;
    const float3 n1 = ix * o1.x + iy * o1.y + iz * o1.z;
    const float3 n2 = ix * o2.x + iy * o2.y + iz * o2.z;
    tri.p0 = w0;
    tri.e1 = w1 - w0;
    tri.e2 = w2 - w0;
    tri.n0 = n0;
    tri.a1 = n1 - n0;
    tri.a2 = n2 - n0;
    tri.c = float3(0.0);
    return tri;
}

// sigma at (u, v) and its derivatives (du: d sigma / du, dv: d sigma / dv); y the point, n its oriented shading
// normal. False where the map is undefined: p on the triangle's plane, a degenerate normal or frame, or H not above
// the normal (no reflection can have it as half vector).
bool manifoldMap(ManifoldTri tri, float3 p, float3 L, float2 uv, out float2 sigma, out float2 du, out float2 dv,
                 out float3 y, out float3 n)
{
    sigma = float2(0.0);
    du = float2(0.0);
    dv = float2(0.0);
    y = tri.p0 + tri.e1 * uv.x + tri.e2 * uv.y;
    n = float3(0.0, 0.0, 1.0);
    const float3 d = p - y;
    const float r = length(d);
    if (!(r > 0.0)) return false;
    const float3 w = d / r;
    const float3 H = w + L;
    const float3 N = tri.n0 + tri.a1 * uv.x + tri.a2 * uv.y;
    const float lenN = length(N);
    if (!(lenN > 1e-12)) return false;
    const float3 Nh = N / lenN;
    // Turned as fillHit turns it: toward the side of the triangle the ray from p meets (the geometric normal
    // toward p).
    const float3 g = cross(tri.e1, tri.e2);
    const float side = dot(g, d) >= 0.0 ? 1.0 : -1.0;
    const float s = dot(Nh, g) * side < 0.0 ? -1.0 : 1.0;
    n = Nh * s;
    const float3 c = tri.c;
    const float3 T = c - n * dot(c, n);
    const float lenT = length(T);
    if (!(lenT > 1e-6)) return false;
    const float3 t = T / lenT;
    const float3 b = cross(n, t);
    const float A = dot(H, t), B = dot(H, b), C = dot(H, n);
    if (!(C > 1e-7)) return false;
    sigma = float2(A / C, B / C);
    [unroll] for (uint k = 0u; k < 2u; ++k)
    {
        const float3 e = k == 0u ? tri.e1 : tri.e2;
        const float3 a = k == 0u ? tri.a1 : tri.a2;
        const float3 dw = -(e - w * dot(w, e)) / r;         // d w (y moves by e; d = p - y moves by -e)
        const float3 dn = s * (a - Nh * dot(Nh, a)) / lenN;
        const float3 dT = -n * dot(c, dn) - dn * dot(c, n);
        const float3 dt = (dT - t * dot(t, dT)) / lenT;
        const float3 db = cross(dn, t) + cross(n, dt);
        const float dA = dot(dw, t) + dot(H, dt);
        const float dB = dot(dw, b) + dot(H, db);
        const float dC = dot(dw, n) + dot(H, dn);
        const float2 ds = float2(dA * C - A * dC, dB * C - B * dC) / (C * C);
        if (k == 0u) du = ds;
        else dv = ds;
    }
    return true;
}

// Newton on sigma(u, v) = target from the triangle's centroid. Converged when |sigma - target| <= tol; uv is then
// the root (which may lie outside the triangle: the caller keeps it only inside). At most 16 steps; a step that
// leaves the triangle's neighbourhood (|u|, |v| over 4) gives up.
bool manifoldSolve(ManifoldTri tri, float3 p, float3 L, float2 target, float tol, out float2 uv)
{
    uv = float2(1.0 / 3.0, 1.0 / 3.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        float2 sigma, du, dv;
        float3 y, n;
        if (!manifoldMap(tri, p, L, uv, sigma, du, dv, y, n)) return false;
        const float2 f = sigma - target;
        if (dot(f, f) <= tol * tol) return true;
        const float det = du.x * dv.y - dv.x * du.y;
        if (!(abs(det) > 1e-20)) return false;
        // J = [du dv] (columns); J^-1 f.
        const float2 step = float2(dv.y * f.x - dv.x * f.y, -du.y * f.x + du.x * f.y) / det;
        uv -= step;
        if (abs(uv.x) > 4.0 || abs(uv.y) > 4.0) return false;
    }
    float2 sigma, du, dv;
    float3 y, n;
    if (!manifoldMap(tri, p, L, uv, sigma, du, dv, y, n)) return false;
    const float2 f = sigma - target;
    return dot(f, f) <= tol * tol;
}

// ---- the slope's density ------------------------------------------------------------------------
// GGX's distribution of slopes at alpha (P22(sigma) = 1 / (pi a^2 (1 + |sigma|^2 / a^2)^2)), truncated to the core's disc
// |sigma|^2 <= R^2 = max(core.x a^2, core.y) (core.x GGX's stretched slope^2, core.y a plain slope^2: the module's
// inCore with alpha the lobe's largest, so the disc holds every core) and renormalised by its mass there,
// R^2 / (a^2 + R^2). With two smooth lobes (a base and a coat, alphas.x and .y), half the draws use each.

float manifoldCoreRadius2(float a, float2 core) { return max(core.x * a * a, core.y); }

float manifoldSlopeLobe(float a, float2 core, float2 sigma)
{
    if (!(a > 0.0)) return 0.0;
    const float a2 = a * a;
    const float R2 = manifoldCoreRadius2(a, core);
    const float s2 = dot(sigma, sigma);
    if (s2 > R2) return 0.0;
    const float k = 1.0 + s2 / a2;
    return (a2 + R2) / (R2 * kManifoldPi * a2 * k * k);
}

float manifoldSlopeDensity(float2 alphas, float2 core, float2 sigma)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    if (b && c) return 0.5 * (manifoldSlopeLobe(alphas.x, core, sigma) + manifoldSlopeLobe(alphas.y, core, sigma));
    return manifoldSlopeLobe(b ? alphas.x : alphas.y, core, sigma);
}

// GGX's slopes at alpha untruncated (step 2b: the camera side renders none of the sun through a solid, so the pass draws
// the whole lobe): P22 and a draw from it.
float manifoldGgxSlopeDensity(float a, float2 sigma)
{
    const float a2 = a * a;
    const float k = 1.0 + dot(sigma, sigma) / a2;
    return 1.0 / (kManifoldPi * a2 * k * k);
}

float2 manifoldGgxSlope(float a, float2 u)
{
    const float xi = min(u.x, 1.0 - 1.0 / 16777216.0);
    const float r = a * sqrt(xi / (1.0 - xi));
    const float phi = 2.0 * kManifoldPi * u.y;
    return float2(r * cos(phi), r * sin(phi));
}

float2 manifoldDrawSlope(float2 alphas, float2 core, float3 u)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    const float a = b && c ? (u.x < 0.5 ? alphas.x : alphas.y) : (b ? alphas.x : alphas.y);
    const float q = manifoldCoreRadius2(a, core) / (a * a);   // the disc's radius^2 in alphas^2
    const float share = q / (1.0 + q);
    const float xi = min(u.y * share, share);
    const float r = a * sqrt(xi / (1.0 - xi));
    const float phi = 2.0 * kManifoldPi * u.z;
    return float2(r * cos(phi), r * sin(phi));
}

// ---- pruning ------------------------------------------------------------------------------------
// Whether a box, every shading normal in it within the cone (axis, its half angle), can hold a point y whose half
// vector between normalize(p - y) and L is at the angle thetaSigma from its normal (either way round the axis: the
// normal is turned toward p). False only when no such point can exist. The directions from the box toward p lie
// within dw of the centre's (asin of the box's radius over its distance); the half vector moves at most
// dw / |w + L| <= dw / (2 cos(gammaMax / 2)), gammaMax the largest angle between w and L.
bool manifoldConeTest(float3 boxMin, float3 boxMax, float3 axis, float halfAngle, float3 p, float3 L, float thetaSigma)
{
    const float3 centre = 0.5 * (boxMin + boxMax);
    const float radius = 0.5 * length(boxMax - boxMin);
    const float3 d = p - centre;
    const float dist = length(d);
    if (!(dist > radius * 1.0001 + 1e-6)) return true;
    const float3 wc = d / dist;
    const float dw = asin(min(radius / dist, 1.0));
    const float gammaMax = acos(clamp(dot(wc, L), -1.0, 1.0)) + dw;
    if (gammaMax >= kManifoldPi - 1e-3) return true;
    const float dh = dw / (2.0 * cos(0.5 * gammaMax));
    const float3 hc = normalize(wc + L);
    const float ang = acos(clamp(dot(hc, axis), -1.0, 1.0));
    const float slack = halfAngle + dh + 1e-4;
    return abs(ang - thetaSigma) <= slack || abs((kManifoldPi - ang) - thetaSigma) <= slack;
}

// A triangle's bounds for the disc test: its box and the cone of its three vertex normals (a normalised convex
// combination of them stays in that cone while it is under 90 degrees; past that the half angle is pi: no pruning).
void manifoldTriangleBounds(ManifoldTri tri, out float3 boxMin, out float3 boxMax, out float3 axis, out float halfAngle)
{
    const float3 p1 = tri.p0 + tri.e1, p2 = tri.p0 + tri.e2;
    boxMin = min(tri.p0, min(p1, p2));
    boxMax = max(tri.p0, max(p1, p2));
    axis = float3(0.0, 0.0, 1.0);
    halfAngle = kManifoldPi;
    const float3 n0 = tri.n0, n1 = tri.n0 + tri.a1, n2 = tri.n0 + tri.a2;
    const float l0 = length(n0), l1 = length(n1), l2 = length(n2);
    if (!(l0 > 1e-12 && l1 > 1e-12 && l2 > 1e-12)) return;
    const float3 u0 = n0 / l0, u1 = n1 / l1, u2 = n2 / l2;
    const float3 s = u0 + u1 + u2;
    if (!(dot(s, s) > 1e-20)) return;
    axis = normalize(s);
    const float h = max(acos(clamp(dot(axis, u0), -1.0, 1.0)),
                        max(acos(clamp(dot(axis, u1), -1.0, 1.0)), acos(clamp(dot(axis, u2), -1.0, 1.0))));
    halfAngle = h >= 0.5 * kManifoldPi ? kManifoldPi : h;
}

// manifoldDiscTest on one triangle (its frame's c).
bool manifoldTriangleTest(ManifoldTri tri, float3 p, float3 L, float2 sigma)
{
    float3 lo, hi, axis;
    float halfAngle;
    manifoldTriangleBounds(tri, lo, hi, axis, halfAngle);
    return manifoldDiscTest(lo, hi, axis, halfAngle, tri.c, sigma, p, L);
}

// The half vector a slope sigma means at normal n in the frame of reference c: along n + sigma.x t + sigma.y b.
float3 manifoldHalfAt(float3 n, float3 c, float2 sigma)
{
    const float3 t = normalize(c - n * dot(c, n));
    const float3 b = cross(n, t);
    return normalize(n + t * sigma.x + b * sigma.y);
}

// A node's term for the disc test, from its cone and frame of reference: halfAngle (1 + 2 K), K = |cot| of the
// normal's angle to c over the cone; -1 when the cone is too wide to prune (the test then keeps the node).
float manifoldDiscTerm(float3 axis, float halfAngle, float3 c)
{
    if (!(halfAngle < 1.2)) return -1.0;
    const float g0 = acos(clamp(dot(c, axis), -1.0, 1.0));
    const float gMin = min(g0 - halfAngle, kManifoldPi - g0 - halfAngle);
    if (!(gMin > 0.05)) return -1.0;
    return halfAngle * (1.0 + 2.0 / tan(gMin));
}

// manifoldDiscTest with the node's term precomputed (manifoldDiscTerm; the gather stores it). The half vector the slope
// means at normal a, (a + sigma.x t + sigma.y b) / sqrt(1 + |sigma|^2), is compared with H = wc + L by dot products
// (either orientation: -a, with b turned). (Loosening its asin and cos to polynomials passed so many more nodes that
// the pass took 1.4x as long: the pruning, not the test, is the cost.)
bool manifoldDiscTestTerm(float3 boxMin, float3 boxMax, float3 axis, float halfAngle, float term, float3 c, float2 sigma,
                          float3 p, float3 L)
{
    if (term < 0.0) return true;
    const float3 centre = 0.5 * (boxMin + boxMax);
    const float radius = 0.5 * length(boxMax - boxMin);
    const float3 d = p - centre;
    const float dist = length(d);
    if (!(dist > radius * 1.0001 + 1e-6)) return true;
    const float3 wc = d / dist;
    const float dw = asin(min(radius / dist, 1.0));
    const float cg = clamp(dot(wc, L), -1.0, 1.0);
    const float gammaMax = acos(cg) + dw;
    if (gammaMax >= kManifoldPi - 1e-3) return true;
    const float dh = dw / (2.0 * cos(0.5 * gammaMax));
    // The half vector the slope means moves with the normal by at most 2 asin(dv / 2), dv = halfAngle + |sigma| term
    // (manifoldDiscTest's bound: halfAngle (1 + |sigma| (1 + 2 K)) <= halfAngle + |sigma| term).
    const float dv = halfAngle + length(sigma) * term;
    if (dv >= 2.0) return true;
    const float slack = 2.0 * asin(0.5 * dv) + dh + 1e-4;
    if (slack >= kManifoldPi) return true;
    const float3 t = normalize(c - axis * dot(c, axis));
    const float3 b = cross(axis, t);
    const float3 H = wc + L;
    const float ha = dot(H, axis), ht = dot(H, t), hb = dot(H, b);
    const float need = cos(slack) * sqrt(max(2.0 + 2.0 * cg, 0.0)) * sqrt(1.0 + dot(sigma, sigma));
    return ha + sigma.x * ht + sigma.y * hb >= need || -ha + sigma.x * ht - sigma.y * hb >= need;
}

// Whether a box whose shading normals lie in a cone (axis, half angle) can hold a point y where the half vector
// between normalize(p - y) and L is the slope sigma in the frame of reference c (either orientation of the normal).
// False only when no such point can exist. The half vector the slope means moves with the normal by at most
// 2 asin(beta (1 + |sigma| (1 + 2 K)) / 2), K = |cot| of the normal's angle to c over the cone (the frame's tangent
// turns by K per radian the normal turns); the half vectors from the box toward p lie within dh of the centre's
// (manifoldConeTest's bound).
bool manifoldDiscTest(float3 boxMin, float3 boxMax, float3 axis, float halfAngle, float3 c, float2 sigma, float3 p, float3 L)
{
    return manifoldDiscTestTerm(boxMin, boxMax, axis, halfAngle, manifoldDiscTerm(axis, halfAngle, c), c, sigma, p, L);
}

// ---- step 2c: a normal-mapped triangle's composed cone ------------------------------------------------------------
// MikkTSpace's frame (material/material_layers.slang, mikkTSpaceNormal; rt/manifold_poly.slang, polyFrameAt): per
// vertex the unit world normal n, the unit world tangent t and w n x t, interpolated as they are, M = [t b n] a 3 x 3
// matrix affine in the triangle's (u, v). The drawn slope sigma (the vertex tangent frame's, cellPolyOf's) means the
// microfacet normal h = m - sigma.x t - sigma.y b = M (tn - s), s = (sigma, 0), tn the map's tangent-space normal there.
//
// The bound. On a region of the triangle (a convex polygon, the hull of its vertices v_k), with M' the frame at their
// mean, delta = max_k |M(v_k) - M'|_F (|M - M'|_F is convex, so the max over the region is at a vertex), g >= |M'|_2,
// and every tn of the region in the ball B(c, rho):
//   |h - M'(c - s)| <= |M'(tn - c)| + |(M - M')(tn - s)| <= g rho + delta (|c - s| + rho) = r,
// so h lies within asin(r / |a|) of a = M'(c - s) when r < |a| (else nothing is pruned). A cell's tn is the bilinear
// blend of its four texels' normals at weights >= 0 summing to 1, so it lies in their convex hull, inside any ball
// holding the four (the pyramid's: render/rt/manifold_casters.hpp). The cone goes to manifoldConeTest at the angle 0
// (h along the half vector from the region's box, either way round), the box's own bound unchanged.

// g >= |M|_2 for M = [mt mb mn]: the square root of Gershgorin's bound on M^T M's largest eigenvalue.
float manifoldFrameNorm(float3 mt, float3 mb, float3 mn)
{
    const float tt = dot(mt, mt), bb = dot(mb, mb), nn = dot(mn, mn);
    const float tb = abs(dot(mt, mb)), tn = abs(dot(mt, mn)), bn = abs(dot(mb, mn));
    return sqrt(max(tt + tb + tn, max(tb + bb + bn, tn + bn + nn))) * (1.0 + 1e-6);
}

// |M1 - M2|_F.
float manifoldFrameDistance(float3 t1, float3 b1, float3 n1, float3 t2, float3 b2, float3 n2)
{
    const float3 dt = t1 - t2, db = b1 - b2, dn = n1 - n2;
    return sqrt(dot(dt, dt) + dot(db, db) + dot(dn, dn));
}

// Whether a region of a mapped triangle (its box; its frame M' = [mt mb mn], delta and g; the ball (c, rho) holding
// its tangent-space normals, rho < 0 for none) can hold a point whose h for the slope sigma is along the half vector
// toward p and the light L. False only when none can. The margins cover fp32's rounding of a and r (a few ulps of
// their terms; manifoldConeTest adds 1e-4 rad).
bool manifoldMappedTest(float3 boxMin, float3 boxMax, float3 mt, float3 mb, float3 mn, float delta, float g, float4 ball,
                        float2 sigma, float3 p, float3 L)
{
    if (ball.w < 0.0 || delta < 0.0) return false;
    const float3 c = ball.xyz - float3(sigma, 0.0);
    const float3 a = mt * c.x + mb * c.y + mn * c.z;
    const float la = length(a), lc = length(c);
    const float r = g * ball.w + delta * (lc + ball.w);
    const float rr = r * (1.0 + 1e-5) + 1e-5 * (g * lc + r) + 1e-12;
    if (!(rr < la)) return true;
    return manifoldConeTest(boxMin, boxMax, a / la, asin(rr / la), p, L, 0.0);
}

// A mapped triangle as the walk reads it (render/rt/manifold_casters.hpp's side table): world corners, MikkTSpace's
// frame at each vertex and the texture coordinates.
struct ManifoldMapTri
{
    float3 p0, e1, e2;
    float3 n0, n1, n2, t0, t1, t2, b0, b1, b2;
    float2 uv0, uv1, uv2;
};

// The frame at the triangle's (u, v).
void manifoldMapFrameAt(ManifoldMapTri tri, float2 w, out float3 t, out float3 b, out float3 n)
{
    t = tri.t0 + (tri.t1 - tri.t0) * w.x + (tri.t2 - tri.t0) * w.y;
    b = tri.b0 + (tri.b1 - tri.b0) * w.x + (tri.b2 - tri.b0) * w.y;
    n = tri.n0 + (tri.n1 - tri.n0) * w.x + (tri.n2 - tri.n0) * w.y;
}

// A region of the triangle for manifoldMappedTest: its box, frame, delta and g.
struct ManifoldMapRegion
{
    float3 lo, hi, mt, mb, mn;
    float delta, g;
};

// The region from its vertices in the triangle's (u, v) (at most 8; n of them).
ManifoldMapRegion manifoldMapRegionOf(ManifoldMapTri tri, float2 w[8], uint n)
{
    ManifoldMapRegion r;
    r.lo = float3(1e30);
    r.hi = float3(-1e30);
    float2 mean = float2(0.0);
    for (uint k = 0u; k < 8u; ++k)
    {
        if (k >= n) break;
        const float3 y = tri.p0 + tri.e1 * w[k].x + tri.e2 * w[k].y;
        r.lo = min(r.lo, y);
        r.hi = max(r.hi, y);
        mean += w[k];
    }
    mean /= float(max(n, 1u));
    manifoldMapFrameAt(tri, mean, r.mt, r.mb, r.mn);
    r.delta = 0.0;
    for (uint k = 0u; k < 8u; ++k)
    {
        if (k >= n) break;
        float3 t, b, nn;
        manifoldMapFrameAt(tri, w[k], t, b, nn);
        r.delta = max(r.delta, manifoldFrameDistance(t, b, nn, r.mt, r.mb, r.mn));
    }
    r.delta = r.delta * (1.0 + 1e-5) + 1e-6;
    r.g = manifoldFrameNorm(r.mt, r.mb, r.mn);
    // The box grown by its corners' rounding (the positions' and the vertices' (u, v)).
    const float3 grow = (abs(r.lo) + abs(r.hi) + abs(tri.e1) + abs(tri.e2)) * 1e-6 + 1e-7;
    r.lo -= grow;
    r.hi += grow;
    return r;
}

// The whole triangle's region.
ManifoldMapRegion manifoldMapRegionTri(ManifoldMapTri tri)
{
    float2 w[8];
    for (uint k = 0u; k < 8u; ++k) w[k] = float2(0.0);
    w[1] = float2(1.0, 0.0);
    w[2] = float2(0.0, 1.0);
    return manifoldMapRegionOf(tri, w, 3u);
}

// The part of the triangle whose texture coordinates lie in the rectangle (x0, y0, x1, y1) (grown by its coordinates'
// rounding): the rectangle clipped by the triangle's three edges in uv (Sutherland and Hodgman), its vertices taken to
// (u, v). False when it is empty. A triangle whose uvs are degenerate (no (u, v) for a uv) is kept whole wherever the
// rectangle meets its uvs' box (`degenerate` set).
bool manifoldMapClip(ManifoldMapTri tri, float4 rect, out ManifoldMapRegion region, out bool degenerate)
{
    const float eps = (1.0 + max(max(abs(rect.x), abs(rect.y)), max(abs(rect.z), abs(rect.w)))) * 4e-6;
    rect += float4(-eps, -eps, eps, eps);
    const float2 q0 = tri.uv0, qa = tri.uv1 - tri.uv0, qb = tri.uv2 - tri.uv0;
    const float det = qa.x * qb.y - qa.y * qb.x;
    const float scale = max(max(abs(qa.x), abs(qa.y)), max(abs(qb.x), abs(qb.y)));
    degenerate = !(abs(det) > 1e-10 * scale * scale) || !(scale > 0.0);
    if (degenerate)
    {
        const float2 lo = min(tri.uv0, min(tri.uv1, tri.uv2)), hi = max(tri.uv0, max(tri.uv1, tri.uv2));
        region = manifoldMapRegionTri(tri);
        return !(lo.x > rect.z || lo.y > rect.w || hi.x < rect.x || hi.y < rect.y);
    }
    float2 poly[8], next[8];
    for (uint k = 0u; k < 8u; ++k)
    {
        poly[k] = float2(0.0);
        next[k] = float2(0.0);
    }
    poly[0] = rect.xy;
    poly[1] = rect.zy;
    poly[2] = rect.zw;
    poly[3] = rect.xw;
    uint n = 4u;
    const float side = det > 0.0 ? 1.0 : -1.0;
    [ForceUnroll]
    for (uint e = 0u; e < 3u; ++e)
    {
        const float2 a = e == 0u ? tri.uv0 : e == 1u ? tri.uv1 : tri.uv2;
        const float2 b = e == 0u ? tri.uv1 : e == 1u ? tri.uv2 : tri.uv0;
        const float2 ab = b - a;
        uint m = 0u;
        for (uint k = 0u; k < 8u; ++k)
        {
            if (k >= n) break;
            const float2 p = poly[k], q = poly[k + 1u < n ? k + 1u : 0u];
            const float dp = side * (ab.x * (p.y - a.y) - ab.y * (p.x - a.x));
            const float dq = side * (ab.x * (q.y - a.y) - ab.y * (q.x - a.x));
            if (dp >= 0.0 && m < 8u) next[m++] = p;
            if ((dp >= 0.0) != (dq >= 0.0) && m < 8u) next[m++] = p + (q - p) * (dp / (dp - dq));
        }
        n = m;
        for (uint k = 0u; k < 8u; ++k) poly[k] = next[k];
    }
    if (n == 0u)
    {
        region = manifoldMapRegionTri(tri);
        return false;
    }
    // uv to (u, v): the inverse of [qa qb].
    float2 w[8];
    for (uint k = 0u; k < 8u; ++k)
    {
        const float2 d = poly[k] - q0;
        w[k] = float2(qb.y * d.x - qb.x * d.y, qa.x * d.y - qa.y * d.x) / det;
    }
    region = manifoldMapRegionOf(tri, w, n);
    return true;
}

// A cell's own test, tighter than its ball's (manifoldMappedTest): the cell's tangent-space normals lie in the convex
// hull of its four texels' (cNN), so its h = M (tn - s) lie in the hull of M v_k, v_k = cNN - s. Seen along the half
// vector Hc from the region's box centre (the half vectors from the box within dh of it: manifoldConeTest's bound), a
// root needs some w in that hull with |w x Hc| <= tan(dh) |w . Hc|. With M = M' + E, |E| <= delta: the hull of M' v_k
// projected on the plane across Hc must come within max_k delta |v_k| + tan(dh) max_k (|M' v_k . Hc| + delta |v_k|) of
// its origin. False only when no point of the region can reflect (exact as the region shrinks: the line test).
float manifoldSegmentDistance(float2 a, float2 b)
{
    const float2 ab = b - a;
    const float t = clamp(-dot(a, ab) / max(dot(ab, ab), 1e-30), 0.0, 1.0);
    return length(a + ab * t);
}

bool manifoldCellHullTest(float3 boxMin, float3 boxMax, float3 mt, float3 mb, float3 mn, float delta, float3 c00,
                          float3 c10, float3 c01, float3 c11, float2 sigma, float3 p, float3 L)
{
    const float3 centre = 0.5 * (boxMin + boxMax);
    const float radius = 0.5 * length(boxMax - boxMin);
    const float3 d = p - centre;
    const float dist = length(d);
    if (!(dist > radius * 1.0001 + 1e-6)) return true;
    const float3 wc = d / dist;
    const float dw = asin(min(radius / dist, 1.0));
    const float gammaMax = acos(clamp(dot(wc, L), -1.0, 1.0)) + dw;
    if (gammaMax >= kManifoldPi - 1e-3) return true;
    const float dh = dw / (2.0 * cos(0.5 * gammaMax)) + 1e-4;
    if (dh >= 1.5) return true;
    const float3 hc = normalize(wc + L);
    const float3 e1 = normalize(cross(hc, abs(hc.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
    const float3 e2 = cross(hc, e1);
    const float3 s = float3(sigma, 0.0);
    float2 P[4];
    float pert = 0.0, depth = 0.0, scale = 0.0;
    [ForceUnroll]
    for (uint k = 0u; k < 4u; ++k)
    {
        const float3 v = (k == 0u ? c00 : k == 1u ? c10 : k == 2u ? c01 : c11) - s;
        const float3 w = mt * v.x + mb * v.y + mn * v.z;
        P[k] = float2(dot(w, e1), dot(w, e2));
        const float lv = length(v);
        pert = max(pert, delta * lv);
        depth = max(depth, abs(dot(w, hc)) + delta * lv);
        scale = max(scale, length(w));
    }
    const float slack = pert + tan(dh) * depth + 1e-5 * scale + 1e-12;
    // Inside one of the four points' triangles (their union is the hull): distance 0.
    [ForceUnroll]
    for (uint k = 0u; k < 4u; ++k)
    {
        // (0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)
        const float2 A = P[k == 3u ? 1u : 0u], B = P[k <= 1u ? 1u : 2u], C = P[k == 0u ? 2u : 3u];
        // A triangle of no area holds nothing its edges (the segments below) do not.
        const float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        if (!(abs(area) > 0.0)) continue;
        const float d1 = (B.x - A.x) * (-A.y) - (B.y - A.y) * (-A.x);
        const float d2 = (C.x - B.x) * (-B.y) - (C.y - B.y) * (-B.x);
        const float d3 = (A.x - C.x) * (-C.y) - (A.y - C.y) * (-C.x);
        if ((d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) || (d1 <= 0.0 && d2 <= 0.0 && d3 <= 0.0)) return true;
    }
    float m = 1e30;
    [ForceUnroll]
    for (uint i = 0u; i < 3u; ++i)
        [ForceUnroll]
        for (uint j = i + 1u; j < 4u; ++j) m = min(m, manifoldSegmentDistance(P[i], P[j]));
    return m <= slack;
}
