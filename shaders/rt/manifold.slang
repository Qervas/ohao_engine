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
