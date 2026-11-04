// Manifold connections (sub-project 24 step 2, docs/specs/2026-11-04-caustics-step2-design.md, "Step 2a as
// designed"): the pieces of the estimator that solve for the point y on a smooth triangle where the sun's light
// reflects toward a rough point p. Pure functions: rt/path_tracer.slang's manifoldConnect calls them, and
// tools/caustics/manifold_kernel.slang runs them on the CPU for tests/renderer/manifold_tests.
//
// The map. On a triangle y(u, v) = p0 + u e1 + v e2, its interpolated shading normal N(u, v) = n0 + u a1 + v a2
// (world normals by the inverse transpose, unnormalised: fillHit's normalize(mul(nObj, w2o)) is the same direction),
// turned to the side the triangle shows p. With w = normalize(p - y) and L toward the light, the half vector is
// H = w + L, and sigma(u, v) is its slope in a frame of the triangle's own: n, t = normalize(c - (c.n) n) with c the
// first edge's direction, b = n x t, sigma = (H.t / H.n, H.b / H.n). Any smooth frame is a valid change of variables
// on one triangle, so a frame per triangle is exact; |sigma| = tan(angle(H, n)) whatever the frame.
//
// The estimator draws sigma (manifoldDrawSlope), solves sigma(u, v) = sigma* on every candidate triangle
// (manifoldSolve), and weighs each root by 1 / (p(sigma) |det d sigma / d(u, v)| / |e1 x e2|) (area per slope).

static const float kManifoldPi = 3.14159265358979;

struct ManifoldTri
{
    float3 p0, e1, e2;   // world corners: p0, p1 - p0, p2 - p0
    float3 n0, a1, a2;   // world vertex normals (unnormalised): n0, n1 - n0, n2 - n0
};

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
    const float3 c = normalize(tri.e1);
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
// GGX's distribution of slopes at alpha (P22(sigma) = 1 / (pi a^2 (1 + |sigma|^2 / a^2)^2)), truncated to the core
// |sigma|^2 <= slope2 a^2 and renormalised by its mass there, slope2 / (1 + slope2). With two smooth lobes (a base
// and a coat, alphas.x and .y), half the draws use each.

float manifoldSlopeLobe(float a, float slope2, float2 sigma)
{
    if (!(a > 0.0)) return 0.0;
    const float q = dot(sigma, sigma) / (a * a);
    if (q > slope2) return 0.0;
    const float k = 1.0 + q;
    return (1.0 + slope2) / (slope2 * kManifoldPi * a * a * k * k);
}

float manifoldSlopeDensity(float2 alphas, float slope2, float2 sigma)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    if (b && c) return 0.5 * (manifoldSlopeLobe(alphas.x, slope2, sigma) + manifoldSlopeLobe(alphas.y, slope2, sigma));
    return manifoldSlopeLobe(b ? alphas.x : alphas.y, slope2, sigma);
}

float2 manifoldDrawSlope(float2 alphas, float slope2, float3 u)
{
    const bool b = alphas.x > 0.0, c = alphas.y > 0.0;
    const float a = b && c ? (u.x < 0.5 ? alphas.x : alphas.y) : (b ? alphas.x : alphas.y);
    const float share = slope2 / (1.0 + slope2);
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
