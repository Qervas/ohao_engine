// Manifold connections (sub-project 24 step 2, docs/specs/2026-11-04-caustics-step2-design.md, "Step 2a as
// designed"): the pieces of the estimator that find the points y on a smooth triangle where the sun's light reflects
// toward a rough point p. Pure functions: rt/path_tracer.slang's manifold rounds call them.
//
// The frame of reference. On a triangle y(u, v) = p0 + u e1 + v e2 with vertex normals n_k (world normals by the inverse
// transpose, unnormalised; the shading normal is their interpolation normalised, turned to the side the triangle shows
// p), each vertex has 2a's frame n_k, t_k = normalize(c - (c.n_k) n_k), b_k = n_k x t_k, c a unit vector of the
// triangle's cluster (perpendicular to its cone's axis). A slope sigma means the microfacet normal
// h = n + sigma.x t + sigma.y b, the frame interpolated as it is (rt/manifold_poly.slang, polyTriangleSource), and a root
// is a y whose half vector H = normalize(p - y) + L lies along h. Any smooth frame is a valid change of variables on one
// triangle, so the choice is exact; one c for a whole cluster makes a slope point the same way across it, which is what
// lets manifoldDiscTest prune to the triangles near one point instead of a ring (|sigma| = tan(angle(H, n)) alone allows
// a ring round a sphere).
//
// The estimator draws sigma (manifoldDrawSlope), finds every root on every candidate triangle (rt/manifold_poly.slang's
// subdivision), and weighs each root by the area a unit of slope covers there over p(sigma) (cellPolyAreaPerSlope).

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

// ---- the slope's density ------------------------------------------------------------------------
// GGX's distribution of slopes at alpha, P22(sigma) = 1 / (pi a^2 (1 + |sigma|^2 / a^2)^2), untruncated: a root's light is
// kept only where its true slope (the half vector's in the shading frame) lies in the core (the connect's
// evalSmoothCore), and the drawn slope is the frame of reference's, which differs from the true slope inside a triangle
// (the vertex frame interpolated: by second order in the normals' spread; a normal map's tilt stretches it by up to its
// inverse cosine), so a draw truncated to the core's disc would miss the part of the core the stretch moves past it.
// The core holds at least 99% of GGX's slopes (kManifoldCoreSlope2), so the draws past it cost at most 1%. With two
// smooth lobes (a base and a coat, alphas.x and .y), half the draws use each.
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

float manifoldSlopeDensity(float2 alphas, float2 sigma)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    if (b && c) return 0.5 * (manifoldGgxSlopeDensity(alphas.x, sigma) + manifoldGgxSlopeDensity(alphas.y, sigma));
    return manifoldGgxSlopeDensity(b ? alphas.x : alphas.y, sigma);
}

float2 manifoldDrawSlope(float2 alphas, float3 u)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    const float a = b && c ? (u.x < 0.5 ? alphas.x : alphas.y) : (b ? alphas.x : alphas.y);
    return manifoldGgxSlope(a, u.yz);
}

// ---- pruning ------------------------------------------------------------------------------------
// Whether a box, every shading normal in it within the cone (axis, its half angle), can hold a point y whose half
// vector between normalize(p - y) and L is at the angle thetaSigma from its normal (either way round the axis when
// `flipped`: the normal is turned toward p). False only when no such point can exist. The directions from the box toward p lie
// within dw of the centre's (asin of the box's radius over its distance); the half vector moves at most
// dw / |w + L| <= dw / (2 cos(gammaMax / 2)), gammaMax the largest angle between w and L.
bool manifoldConeTest(float3 boxMin, float3 boxMax, float3 axis, float halfAngle, float3 p, float3 L, float thetaSigma,
                      bool flipped)
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
    return abs(ang - thetaSigma) <= slack || (flipped && abs((kManifoldPi - ang) - thetaSigma) <= slack);
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
// (h along the half vector from the region's box; either way round unless `oriented`, a path's root: h . H > 0), the
// box's own bound unchanged. Used where the region's vertices are not at hand (the gather's nodes); a triangle and the
// pyramid's regions take the product hull (manifoldMapHullTest), which has no delta.

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
                        float2 sigma, float3 p, float3 L, bool oriented)
{
    if (ball.w < 0.0 || delta < 0.0) return false;
    const float3 c = ball.xyz - float3(sigma, 0.0);
    const float3 a = mt * c.x + mb * c.y + mn * c.z;
    const float la = length(a), lc = length(c);
    const float r = g * ball.w + delta * (lc + ball.w);
    const float rr = r * (1.0 + 1e-5) + 1e-5 * (g * lc + r) + 1e-12;
    if (!(rr < la)) return true;
    return manifoldConeTest(boxMin, boxMax, a / la, asin(rr / la), p, L, 0.0, !oriented);
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

// A region of a mapped triangle: a convex polygon, its n <= 8 vertices in the triangle's (u, v), and its world box
// (grown by its corners' rounding: the positions' and the vertices' (u, v)).
struct ManifoldMapPoly
{
    float2 w[8];
    uint n;
    float3 lo, hi;
};

void manifoldMapPolyBox(ManifoldMapTri tri, inout ManifoldMapPoly poly)
{
    poly.lo = float3(1e30);
    poly.hi = float3(-1e30);
    for (uint k = 0u; k < 8u; ++k)
    {
        if (k >= poly.n) break;
        const float3 y = tri.p0 + tri.e1 * poly.w[k].x + tri.e2 * poly.w[k].y;
        poly.lo = min(poly.lo, y);
        poly.hi = max(poly.hi, y);
    }
    const float3 grow = (abs(poly.lo) + abs(poly.hi) + abs(tri.e1) + abs(tri.e2)) * 1e-6 + 1e-7;
    poly.lo -= grow;
    poly.hi += grow;
}

// The whole triangle.
ManifoldMapPoly manifoldMapPolyTri(ManifoldMapTri tri)
{
    ManifoldMapPoly poly;
    for (uint k = 0u; k < 8u; ++k) poly.w[k] = float2(0.0);
    poly.w[1] = float2(1.0, 0.0);
    poly.w[2] = float2(0.0, 1.0);
    poly.n = 3u;
    manifoldMapPolyBox(tri, poly);
    return poly;
}

// The part of the triangle whose texture coordinates lie in the rectangle (x0, y0, x1, y1), grown by its coordinates'
// rounding: the triangle (0, 0), (1, 0), (0, 1) in (u, v) clipped by the rectangle's four sides, each a half-plane in
// (u, v) because uv(u, v) = uv0 + u (uv1 - uv0) + v (uv2 - uv0) is affine (Sutherland and Hodgman; at most 7 vertices).
// Exact for every triangle, a uv-degenerate one too (its uvs on a segment or a point: a side whose uv does not vary
// over the triangle keeps it whole or drops it), so such a triangle's region is the strip of it whose uvs cross the
// rectangle, never more. False when it is empty.
bool manifoldMapClip(ManifoldMapTri tri, float4 rect, out ManifoldMapPoly poly)
{
    const float eps = (1.0 + max(max(abs(rect.x), abs(rect.y)), max(abs(rect.z), abs(rect.w)))) * 4e-6;
    rect += float4(-eps, -eps, eps, eps);
    const float2 qa = tri.uv1 - tri.uv0, qb = tri.uv2 - tri.uv0;
    float2 next[8];
    for (uint k = 0u; k < 8u; ++k)
    {
        poly.w[k] = float2(0.0);
        next[k] = float2(0.0);
    }
    poly.w[1] = float2(1.0, 0.0);
    poly.w[2] = float2(0.0, 1.0);
    uint n = 3u;
    [ForceUnroll]
    for (uint e = 0u; e < 4u; ++e)
    {
        // Side e as f(w) = o + w.x ga + w.y gb >= 0.
        const float o = e == 0u ? tri.uv0.x - rect.x : e == 1u ? rect.z - tri.uv0.x : e == 2u ? tri.uv0.y - rect.y : rect.w - tri.uv0.y;
        const float ga = e == 0u ? qa.x : e == 1u ? -qa.x : e == 2u ? qa.y : -qa.y;
        const float gb = e == 0u ? qb.x : e == 1u ? -qb.x : e == 2u ? qb.y : -qb.y;
        uint m = 0u;
        for (uint k = 0u; k < 8u; ++k)
        {
            if (k >= n) break;
            const float2 p = poly.w[k], q = poly.w[k + 1u < n ? k + 1u : 0u];
            const float dp = o + p.x * ga + p.y * gb, dq = o + q.x * ga + q.y * gb;
            if (dp >= 0.0 && m < 8u) next[m++] = p;
            if ((dp >= 0.0) != (dq >= 0.0) && m < 8u) next[m++] = p + (q - p) * (dp / (dp - dq));
        }
        n = m;
        for (uint k = 0u; k < 8u; ++k) poly.w[k] = next[k];
    }
    poly.n = n;
    manifoldMapPolyBox(tri, poly);
    return n > 0u;
}

// Whether the convex hull of the 2D points P[0, n) comes within `slack` of the origin. False only on a certificate: a
// direction v with P_k . v / |v| > slack for every k (a line separating the hull from the disc). Found by Gilbert's
// distance algorithm (GJK) in the plane, at most 16 steps, each a support point over the points; true when the hull
// holds the origin, when a point of it is within slack, or when the steps run out (nothing pruned without the proof).
bool manifoldHullNear(float2 P[32], uint n, float slack)
{
    float2 A = P[0], B = P[0];
    uint simplex = 1u;
    float2 v = A;
    for (uint it = 0u; it < 16u; ++it)
    {
        const float vv = dot(v, v);
        if (vv <= slack * slack) return true;
        float lowest = 1e30;
        float2 S = P[0];
        [ForceUnroll]
        for (uint k = 0u; k < 32u; ++k)
        {
            const float d = dot(P[k], v);
            if (k < n && d < lowest)
            {
                lowest = d;
                S = P[k];
            }
        }
        if (lowest > slack * sqrt(vv)) return false;
        if (vv - lowest <= 1e-6 * vv) return true;
        if (simplex == 1u)
        {
            const float2 AS = S - A;
            const float t = clamp(-dot(A, AS) / max(dot(AS, AS), 1e-30), 0.0, 1.0);
            v = A + AS * t;
            if (t >= 1.0) A = S;
            else if (t > 0.0)
            {
                B = S;
                simplex = 2u;
            }
            continue;
        }
        // The triangle A, B, S: holding the origin, or its closest edge (or vertex) the next simplex.
        const float area = (B.x - A.x) * (S.y - A.y) - (B.y - A.y) * (S.x - A.x);
        if (abs(area) > 0.0)
        {
            const float d1 = (B.x - A.x) * (-A.y) - (B.y - A.y) * (-A.x);
            const float d2 = (S.x - B.x) * (-B.y) - (S.y - B.y) * (-B.x);
            const float d3 = (A.x - S.x) * (-S.y) - (A.y - S.y) * (-S.x);
            if ((d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) || (d1 <= 0.0 && d2 <= 0.0 && d3 <= 0.0)) return true;
        }
        const float2 AS = S - A, BS = S - B;
        const float ta = clamp(-dot(A, AS) / max(dot(AS, AS), 1e-30), 0.0, 1.0);
        const float tb = clamp(-dot(B, BS) / max(dot(BS, BS), 1e-30), 0.0, 1.0);
        const float2 va = A + AS * ta, vb = B + BS * tb;
        if (dot(va, va) <= dot(vb, vb))
        {
            v = va;
            if (ta >= 1.0) { A = S; simplex = 1u; }
            else if (ta > 0.0) B = S;
            else simplex = 1u;
        }
        else
        {
            v = vb;
            if (tb >= 1.0) { A = S; simplex = 1u; }
            else if (tb > 0.0) A = S;
            else { A = B; simplex = 1u; }
        }
    }
    return true;
}

// The product hull: whether a region of a mapped triangle (a polygon of its (u, v)) can hold a point y whose h is along
// the half vector toward p and the light L, its tangent-space normals in the hull of the nc <= 4 normals c[j] grown by
// the radius rho (a cell: its four texels, rho 0; a node: its ball's centre and radius; rho < 0: the node holds no
// cell). False only when none can.
//
// The bound. M(y) is affine on the region, so at a point y = sum_k lambda_k y_k of it (lambda convex weights over the
// region's vertices) M(y) = sum_k lambda_k M_k, M_k = M(y_k); and tn = sum_j mu_j c_j + e, |e| <= rho (mu convex
// weights: the bilinear blend of the texels, or the ball). Then
//   h = M(y) (tn - s) = sum_k sum_j lambda_k mu_j M_k (c_j - s) + M(y) e,
// lambda_k mu_j convex weights over the pairs: h lies in the hull of the products P_kj = M_k (c_j - s), grown by
// R = rho max_k |M_k|_2 (|M|_2 is convex, so its largest over the region is at a vertex). Exact for the frame's
// variation, with no slack for it (manifoldMappedTest's delta (|c - s| + rho) is the gather's nodes' only).
//
// Seen along Hc, the half vector from the region's box centre (every point's half vector within dh of it:
// manifoldConeTest's bound), a root needs h = a + e', a in the hull, |e'| <= R, with |h x Hc| <= tan(dh) (h . Hc), so
// the hull projected across Hc comes within tan(dh) (D + R) + R of the origin, D = max_k |P_kj . Hc|. Oriented (a
// path's root: h . H > 0), h . Hc > 0 is needed, so D = max_kj P_kj . Hc, and the region is pruned when D + R <= 0.
// (A root with h along H, either way round, reflects d onto +L: r = d (h . h) - 2 (d . h) h = 2 (1 + w . L) |d| L
// times a positive factor, so r . L > 0 holds at every root this test keeps; the oriented test is the only sign test.)
bool manifoldMapHullTest(ManifoldMapTri tri, ManifoldMapPoly poly, float3 c[4], uint nc, float rho, float2 sigma,
                         float3 p, float3 L, bool oriented)
{
    if (rho < 0.0) return false;
    const float3 centre = 0.5 * (poly.lo + poly.hi);
    const float radius = 0.5 * length(poly.hi - poly.lo);
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
    float2 P[32];
    float depth = -1e30, scale = 0.0, g = 0.0;
    [ForceUnroll]
    for (uint k = 0u; k < 8u; ++k)
    {
        float3 mt, mb, mn;
        manifoldMapFrameAt(tri, poly.w[k < poly.n ? k : 0u], mt, mb, mn);
        if (k < poly.n) g = max(g, manifoldFrameNorm(mt, mb, mn));
        [ForceUnroll]
        for (uint j = 0u; j < 4u; ++j)
        {
            const float3 v = c[j < nc ? j : 0u] - s;
            const float3 w = mt * v.x + mb * v.y + mn * v.z;
            P[4u * k + j] = float2(dot(w, e1), dot(w, e2));
            const float a = dot(w, hc);
            if (k < poly.n && j < nc)
            {
                depth = max(depth, oriented ? a : abs(a));
                scale = max(scale, length(w));
            }
        }
    }
    const float R = rho > 0.0 ? rho * g : 0.0;
    // fp32's rounding of the products and their projections: a few ulps of their size.
    const float round = 1e-5 * (scale + R) + 1e-12;
    if (oriented && depth + R + round <= 0.0) return false;
    // The points in order k-major, nc a vertex: the first poly.n * 4 hold the pairs (a j past nc repeats c[0]).
    const float slack = tan(dh) * (max(depth, 0.0) + R) + R + round;
    return manifoldHullNear(P, 4u * poly.n, slack);
}
