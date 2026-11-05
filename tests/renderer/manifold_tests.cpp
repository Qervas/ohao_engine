// Caustics step 2a's manifold connections (sub-project 24, docs/plans/2026-11-04-caustics-step2a.md, T2):
// shaders/rt/manifold.slang run on the CPU through its slangc C++ twin (tools/caustics/manifold_kernel.slang), held
// to an independent double-precision copy of the map: its value and Jacobian (by central differences), every root
// Newton finds against a search from many starts, the slope draw against its density, and the cone test never
// rejecting a box that holds a root.
#include <gtest/gtest.h>

#include "manifold_cpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace ohao::caustics;

namespace {

constexpr double kPi = 3.14159265358979323846;

using D3 = std::array<double, 3>;
D3 d3(const V3& v) { return {v[0], v[1], v[2]}; }
V3 f3(const D3& v) { return {float(v[0]), float(v[1]), float(v[2])}; }
D3 add(D3 a, D3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 sub(D3 a, D3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 mul(D3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(D3 a, D3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(D3 a, D3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) { return mul(a, 1.0 / len(a)); }

// The map in double, written from the module's description (not its code path).
bool sigmaD(const Tri& t, const D3& p, const D3& L, double u, double v, double out[2], D3* yOut = nullptr) {
    const D3 e1 = d3(t.e1), e2 = d3(t.e2);
    const D3 y = add(d3(t.p0), add(mul(e1, u), mul(e2, v)));
    if (yOut) *yOut = y;
    const D3 w = norm(sub(p, y));
    const D3 H = add(w, L);
    D3 n = norm(add(d3(t.n0), add(mul(d3(t.a1), u), mul(d3(t.a2), v))));
    const D3 g = cross(e1, e2);
    const double side = dot(g, sub(p, y)) >= 0.0 ? 1.0 : -1.0;
    if (dot(n, g) * side < 0.0) n = mul(n, -1.0);
    const D3 c = d3(t.c);
    const D3 tt = norm(sub(c, mul(n, dot(c, n))));
    const D3 b = cross(n, tt);
    const double C = dot(H, n);
    if (!(C > 1e-9)) return false;
    out[0] = dot(H, tt) / C;
    out[1] = dot(H, b) / C;
    return true;
}

double rnd(std::mt19937& g) { return std::uniform_real_distribution<double>(0.0, 1.0)(g); }
D3 sphereDir(std::mt19937& g) {
    const double z = 2.0 * rnd(g) - 1.0, phi = 2.0 * kPi * rnd(g), s = std::sqrt(1.0 - z * z);
    return {s * std::cos(phi), s * std::sin(phi), z};
}

// A unit vector perpendicular to a (the gather's choice for a cluster's frame).
D3 perpendicular(const D3& a) {
    const D3 k = std::abs(a[0]) < 0.9 ? D3{1, 0, 0} : D3{0, 1, 0};
    return norm(cross(a, k));
}

Tri makeTri(const D3& p0, const D3& p1, const D3& p2, const D3& n0, const D3& n1, const D3& n2) {
    const D3 f = norm(add(add(norm(n0), norm(n1)), norm(n2)));
    Tri t{f3(p0), f3(sub(p1, p0)), f3(sub(p2, p0)), f3(n0), f3(sub(n1, n0)), f3(sub(n2, n0))};
    t.c = f3(perpendicular(f));
    return t;
}

// A random small triangle near the origin, its vertex normals tilted up to 0.3 from its face's.
Tri randomTri(std::mt19937& g, double size) {
    const D3 p0 = mul(sphereDir(g), 0.2 * rnd(g));
    const D3 p1 = add(p0, mul(sphereDir(g), size * (0.5 + rnd(g))));
    const D3 p2 = add(p0, mul(sphereDir(g), size * (0.5 + rnd(g))));
    const D3 f = norm(cross(sub(p1, p0), sub(p2, p0)));
    auto tilt = [&] { return norm(add(f, mul(sphereDir(g), 0.3 * rnd(g)))); };
    return makeTri(p0, p1, p2, tilt(), tilt(), tilt());
}

// A UV sphere as the engine generates one (radius r, sectors x stacks), vertex normals radial.
std::vector<Tri> uvSphere(double r, int sectors, int stacks) {
    auto vert = [&](int i, int j) {
        const double th = kPi * double(j) / stacks, ph = 2.0 * kPi * double(i) / sectors;
        return D3{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
    };
    std::vector<Tri> tris;
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < sectors; ++i) {
            const D3 a = vert(i, j), b = vert(i + 1, j), c = vert(i, j + 1), d = vert(i + 1, j + 1);
            if (j != 0) tris.push_back(makeTri(mul(a, r), mul(c, r), mul(b, r), a, c, b));
            if (j != stacks - 1) tris.push_back(makeTri(mul(b, r), mul(c, r), mul(d, r), b, c, d));
        }
    return tris;
}

// Roots of sigma(u, v) = target inside the triangle, by Newton in double from a 6 x 6 grid of starts (finite
// difference Jacobian), merged within 1e-5. Roots within `margin` of an edge are reported apart (either side may
// claim them in float).
struct Roots {
    std::vector<std::array<double, 2>> inside, nearEdge;
};
Roots referenceRoots(const Tri& t, const D3& p, const D3& L, const double target[2], double margin, int grid = 6) {
    Roots r;
    for (int a = 0; a < grid; ++a)
        for (int b = 0; a + b < grid; ++b) {
            double u = (a + 0.5) / (grid + 0.5), v = (b + 0.5) / (grid + 0.5);
            bool conv = false;
            for (int it = 0; it < 60; ++it) {
                double s[2], su[2], sv[2];
                if (!sigmaD(t, p, L, u, v, s)) break;
                const double f0 = s[0] - target[0], f1 = s[1] - target[1];
                if (f0 * f0 + f1 * f1 < 1e-24) {
                    conv = true;
                    break;
                }
                const double h = 1e-7;
                if (!sigmaD(t, p, L, u + h, v, su) || !sigmaD(t, p, L, u, v + h, sv)) break;
                const double j00 = (su[0] - s[0]) / h, j10 = (su[1] - s[1]) / h, j01 = (sv[0] - s[0]) / h,
                             j11 = (sv[1] - s[1]) / h;
                const double det = j00 * j11 - j01 * j10;
                if (std::abs(det) < 1e-30) break;
                u -= (j11 * f0 - j01 * f1) / det;
                v -= (-j10 * f0 + j00 * f1) / det;
                if (std::abs(u) > 4 || std::abs(v) > 4) break;
            }
            if (!conv) continue;
            const double m = std::min({u, v, 1.0 - u - v});
            if (m < -margin) continue;
            auto& list = m > margin ? r.inside : r.nearEdge;
            bool dup = false;
            for (auto& q : r.inside) dup = dup || (std::abs(q[0] - u) < 1e-5 && std::abs(q[1] - v) < 1e-5);
            for (auto& q : r.nearEdge) dup = dup || (std::abs(q[0] - u) < 1e-5 && std::abs(q[1] - v) < 1e-5);
            if (!dup) list.push_back({u, v});
        }
    return r;
}

} // namespace

// The kernel's sigma is the double map's, and its derivatives are the double map's central differences.
TEST(Manifold, t8_jacobian) {
    std::mt19937 g(81u);
    int checked = 0;
    for (int k = 0; k < 2000; ++k) {
        const Tri t = randomTri(g, 0.02 + 0.3 * rnd(g));
        const D3 p = mul(sphereDir(g), 1.0 + 3.0 * rnd(g));
        const D3 L = sphereDir(g);
        double u = rnd(g), v = rnd(g);
        if (u + v > 1.0) {
            u = 1.0 - u;
            v = 1.0 - v;
        }
        const MapResult m = manifoldMap(t, f3(p), f3(L), {float(u), float(v)});
        double s[2];
        if (!sigmaD(t, p, L, u, v, s) || s[0] * s[0] + s[1] * s[1] > 4.0) continue;   // past 63 degrees: grazing
        ASSERT_TRUE(m.ok);
        const double h = 1e-6;
        double a[2], b[2], c[2], d[2];
        ASSERT_TRUE(sigmaD(t, p, L, u + h, v, a) && sigmaD(t, p, L, u - h, v, b) && sigmaD(t, p, L, u, v + h, c) &&
                    sigmaD(t, p, L, u, v - h, d));
        const double J[4] = {(a[0] - b[0]) / (2 * h), (a[1] - b[1]) / (2 * h), (c[0] - d[0]) / (2 * h),
                             (c[1] - d[1]) / (2 * h)};
        const double got[4] = {m.du[0], m.du[1], m.dv[0], m.dv[1]};
        double scale = 0.0;
        for (double x : J) scale = std::max(scale, std::abs(x));
        for (int i = 0; i < 4; ++i)
            EXPECT_NEAR(got[i], J[i], 2e-3 * scale + 1e-5) << "case " << k << " entry " << i;
        for (int i = 0; i < 2; ++i) EXPECT_NEAR(m.sigma[i], s[i], 1e-5 * (1.0 + std::abs(s[i])));
        ++checked;
    }
    EXPECT_GT(checked, 800);
}

// On the showcase's ball (96 x 48), Newton from each triangle's centroid finds every root the search from 21 starts
// finds, and no other, for random views, lights and slopes in a glossy core.
TEST(Manifold, t8_roots_sphere) {
    const std::vector<Tri> tris = uvSphere(0.5, 96, 48);
    std::mt19937 g(82u);
    int roots = 0, cases = 0;
    for (int k = 0; k < 120; ++k) {
        const D3 p = mul(sphereDir(g), 1.0 + 2.0 * rnd(g));
        const D3 L = sphereDir(g);
        const double r = 0.1 * std::sqrt(rnd(g)), phi = 2.0 * kPi * rnd(g);
        const double target[2] = {r * std::cos(phi), r * std::sin(phi)};
        const float tol = 1e-5f;
        for (size_t i = 0; i < tris.size(); ++i) {
            const Tri& t = tris[i];
            const Roots ref = referenceRoots(t, p, L, target, 1e-4);
            V2 uv;
            const bool got = manifoldSolve(t, f3(p), f3(L), {float(target[0]), float(target[1])}, tol, uv);
            const bool gotInside = got && uv[0] >= 0.0f && uv[1] >= 0.0f && uv[0] + uv[1] <= 1.0f;
            ASSERT_LE(ref.inside.size(), 1u) << "two roots in one triangle: Newton from one start cannot hold";
            if (!ref.inside.empty()) {
                ++roots;
                ASSERT_TRUE(gotInside) << "missed a root: case " << k << " triangle " << i;
                EXPECT_NEAR(uv[0], ref.inside[0][0], 1e-3);
                EXPECT_NEAR(uv[1], ref.inside[0][1], 1e-3);
            } else if (ref.nearEdge.empty()) {
                EXPECT_FALSE(gotInside) << "a root no search finds: case " << k << " triangle " << i;
            }
        }
        ++cases;
    }
    EXPECT_GT(roots, 60);
}

// The triangle test keeps every triangle holding a root (on the sphere, at the roots t8_roots_sphere's search finds).
TEST(Manifold, t8_triangle_test_conservative) {
    const std::vector<Tri> tris = uvSphere(0.5, 96, 48);
    std::mt19937 g(86u);
    int roots = 0, pruned = 0;
    for (int k = 0; k < 120; ++k) {
        const D3 p = mul(sphereDir(g), 1.0 + 2.0 * rnd(g));
        const D3 L = sphereDir(g);
        const double r = 0.1 * std::sqrt(rnd(g)), phi = 2.0 * kPi * rnd(g);
        const double target[2] = {r * std::cos(phi), r * std::sin(phi)};
        for (size_t i = 0; i < tris.size(); ++i) {
            const bool keep = manifoldTriangleTest(tris[i], f3(p), f3(L), {float(target[0]), float(target[1])});
            if (!keep) ++pruned;
            const Roots ref = referenceRoots(tris[i], p, L, target, -1e-3);
            if (!ref.inside.empty() || !ref.nearEdge.empty()) {
                ++roots;
                ASSERT_TRUE(keep) << "pruned a triangle holding a root: case " << k << " triangle " << i;
            }
        }
    }
    EXPECT_GT(roots, 60);
    EXPECT_GT(pruned, int(0.995 * 120 * tris.size())) << "it prunes all but a few triangles of a sphere";
}

// A flat mirror (two triangles of one quad): sigma = 0 has exactly one root, the mirror point.
TEST(Manifold, t8_roots_mirror) {
    const D3 up{0, 0, 1};
    const Tri a = makeTri({-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, up, up, up);
    const Tri b = makeTri({-1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, up, up, up);
    std::mt19937 g(83u);
    for (int k = 0; k < 200; ++k) {
        const D3 p{rnd(g) - 0.5, rnd(g) - 0.5, 0.5 + 2.0 * rnd(g)};
        D3 L = sphereDir(g);
        if (L[2] < 0.2) L[2] = 0.2 + rnd(g);
        L = norm(L);
        // The mirror point: w = p - y along L reflected, R = (-Lx, -Ly, Lz).
        const D3 R{-L[0], -L[1], L[2]};
        const D3 y = sub(p, mul(R, p[2] / R[2]));
        const bool onQuad = std::abs(y[0]) < 0.999 && std::abs(y[1]) < 0.999 && std::abs(y[0] - y[1]) > 1e-3;
        int found = 0;
        D3 at{};
        for (const Tri* t : {&a, &b}) {
            V2 uv;
            if (manifoldSolve(*t, f3(p), f3(L), {0.0f, 0.0f}, 1e-6f, uv) && uv[0] >= 0 && uv[1] >= 0 &&
                uv[0] + uv[1] <= 1) {
                ++found;
                at = add(d3(t->p0), add(mul(d3(t->e1), uv[0]), mul(d3(t->e2), uv[1])));
            }
        }
        if (!onQuad) continue;
        ASSERT_EQ(found, 1) << "case " << k;
        EXPECT_LT(len(sub(at, y)), 1e-4);
    }
}

// The slope draw: its density is the formula's, the mass F = q / (1 + q) / share of the drawn |sigma|^2 / a^2 = q is
// uniform, the azimuth uniform; the density integrates to 1 over the core's disc, R^2 = max(slope2 a^2, minSlope2)
// (a mirror's disc is the fixed slope's); a mixture's halves.
TEST(Manifold, t8_slope_density) {
    const float slope2 = 99.0f, minSlope2 = 0.01f;
    for (V2 alphas : {V2{0.01f, 0.0f}, V2{0.0f, 0.0004f}, V2{1e-4f, 0.0f}, V2{0.0625f, 0.0f}}) {
        const double a = alphas[0] > 0 ? alphas[0] : alphas[1];
        const double R2 = std::max(double(slope2) * a * a, double(minSlope2));
        const double qMax = R2 / (a * a), share = qMax / (1.0 + qMax);
        std::mt19937 g(84u);
        const int N = 64000, B = 16;
        std::vector<int> hr(B, 0), hp(B, 0);
        for (int i = 0; i < N; ++i) {
            float dens;
            const V3 u{float(rnd(g)), float(rnd(g)), float(rnd(g))};
            const V2 s = manifoldDrawSlope(alphas, slope2, minSlope2, u, dens);
            const double q = (double(s[0]) * s[0] + double(s[1]) * s[1]) / (a * a);
            ASSERT_LE(q, qMax * 1.0001);
            const double want = 1.0 / (kPi * a * a * (1 + q) * (1 + q)) / share;
            ASSERT_NEAR(dens, want, 1e-3 * want);
            const double F = q / (1 + q) / share;
            hr[std::min(B - 1, int(F * B))]++;
            const double phi = std::atan2(s[1], s[0]) / (2 * kPi) + 0.5;
            hp[std::min(B - 1, int(phi * B))]++;
        }
        double c2r = 0, c2p = 0;
        for (int b = 0; b < B; ++b) {
            const double e = double(N) / B;
            c2r += (hr[b] - e) * (hr[b] - e) / e;
            c2p += (hp[b] - e) * (hp[b] - e) / e;
        }
        EXPECT_LT(c2r, 37.7);   // chi-square, 15 degrees of freedom, p = 0.001
        EXPECT_LT(c2p, 37.7);
        // The density's integral over the disc, in polar steps of |sigma| (squared spacing: the peak is at 0).
        double sum = 0.0;
        const double R = std::sqrt(R2);
        const int steps = 40000;
        for (int i = 0; i < steps; ++i) {
            const double r0 = R * std::pow(double(i) / steps, 3.0), r1 = R * std::pow(double(i + 1) / steps, 3.0);
            const double rm = 0.5 * (r0 + r1);
            sum += manifoldSlopeDensity(alphas, slope2, minSlope2, {float(rm), 0.0f}) * 2 * kPi * rm * (r1 - r0);
        }
        EXPECT_NEAR(sum, 1.0, 2e-3) << "alpha " << a;
        EXPECT_EQ(manifoldSlopeDensity(alphas, slope2, minSlope2, {float(R * 1.01), 0.0f}), 0.0f) << "nothing past the core";
    }
    // A mixture: half its mass at each lobe's alpha.
    const V2 mix{0.01f, 0.0004f};
    const float both = manifoldSlopeDensity(mix, slope2, minSlope2, {0.0f, 0.0f});
    const float b = manifoldSlopeDensity({0.01f, 0.0f}, slope2, minSlope2, {0.0f, 0.0f});
    const float c = manifoldSlopeDensity({0.0f, 0.0004f}, slope2, minSlope2, {0.0f, 0.0f});
    EXPECT_NEAR(both, 0.5f * (b + c), 1e-3f * both);
}

// The cone test never rejects a box holding a point whose normal is in the cone and whose half vector is at the
// asked angle from it; and it does reject (it prunes something).
TEST(Manifold, t8_cone_conservative) {
    std::mt19937 g(85u);
    int rejected = 0;
    for (int k = 0; k < 200000; ++k) {
        const D3 centre = mul(sphereDir(g), rnd(g));
        const double size = 0.01 + 0.3 * rnd(g);
        const D3 half{size * rnd(g), size * rnd(g), size * rnd(g)};
        const D3 lo = sub(centre, half), hi = add(centre, half);
        const D3 y{lo[0] + (hi[0] - lo[0]) * rnd(g), lo[1] + (hi[1] - lo[1]) * rnd(g), lo[2] + (hi[2] - lo[2]) * rnd(g)};
        const D3 axis = sphereDir(g);
        const double halfAngle = 0.3 * rnd(g);
        // a normal in the cone, either way round
        D3 n = norm(add(axis, mul(sphereDir(g), std::tan(halfAngle) * rnd(g) * 0.99)));
        if (std::acos(std::clamp(dot(n, axis), -1.0, 1.0)) > halfAngle) n = axis;
        if (rnd(g) < 0.5) n = mul(n, -1.0);
        const D3 p = add(centre, mul(sphereDir(g), 0.4 + 3.0 * rnd(g)));
        const D3 L = sphereDir(g);
        const D3 h = norm(add(norm(sub(p, y)), L));
        const double theta = std::acos(std::clamp(dot(h, n), -1.0, 1.0));
        ASSERT_TRUE(manifoldConeTest(f3(lo), f3(hi), f3(axis), float(halfAngle), f3(p), f3(L), float(theta)))
            << "case " << k;
        // an angle well away from it: rejected sometimes
        const double other = std::fmod(theta + 1.0, kPi);
        if (!manifoldConeTest(f3(lo), f3(hi), f3(axis), float(halfAngle), f3(p), f3(L), float(other))) ++rejected;
    }
    EXPECT_GT(rejected, 20000);
}

// The disc test never rejects a box holding a point whose normal (in the cone, either way round) and half vector have
// the asked slope in the frame of reference c; and it prunes far more than the ring test.
TEST(Manifold, t8_disc_conservative) {
    std::mt19937 g(87u);
    int rejected = 0, cases = 0;
    for (int k = 0; k < 200000; ++k) {
        const D3 centre = mul(sphereDir(g), rnd(g));
        const double size = 0.01 + 0.3 * rnd(g);
        const D3 half{size * rnd(g), size * rnd(g), size * rnd(g)};
        const D3 lo = sub(centre, half), hi = add(centre, half);
        const D3 y{lo[0] + (hi[0] - lo[0]) * rnd(g), lo[1] + (hi[1] - lo[1]) * rnd(g), lo[2] + (hi[2] - lo[2]) * rnd(g)};
        const D3 axis = sphereDir(g);
        const double halfAngle = 0.6 * rnd(g);
        const D3 c = norm(add(perpendicular(axis), mul(sphereDir(g), 0.3 * rnd(g))));
        D3 n = norm(add(axis, mul(sphereDir(g), std::tan(halfAngle) * rnd(g) * 0.99)));
        if (std::acos(std::clamp(dot(n, axis), -1.0, 1.0)) > halfAngle) n = axis;
        if (rnd(g) < 0.5) n = mul(n, -1.0);
        const D3 p = add(centre, mul(sphereDir(g), 0.4 + 3.0 * rnd(g)));
        const D3 L = sphereDir(g);
        const D3 h = norm(add(norm(sub(p, y)), L));
        if (dot(h, n) < 0.3) continue;
        const D3 t = norm(sub(c, mul(n, dot(c, n))));
        const D3 b = cross(n, t);
        const V2 sigma{float(dot(h, t) / dot(h, n)), float(dot(h, b) / dot(h, n))};
        ++cases;
        ASSERT_TRUE(manifoldDiscTest(f3(lo), f3(hi), f3(axis), float(halfAngle), f3(c), sigma, f3(p), f3(L))) << "case " << k;
        const V2 other{sigma[1] + 0.3f, -sigma[0]};
        if (!manifoldDiscTest(f3(lo), f3(hi), f3(axis), float(halfAngle), f3(c), other, f3(p), f3(L))) ++rejected;
    }
    EXPECT_GT(cases, 50000);
    EXPECT_GT(rejected, cases / 4);
}

// ---- step 2b T1: certified roots (rt/manifold_cert.slang) -------------------------------------------------

// The interval map over a box holds the float map and its Jacobian at every point of the box, and where it says the
// map is undefined over the box no point has it defined (outward rounding holds).
TEST(Manifold, t9_interval_enclosure) {
    std::mt19937 g(91u);
    int boxes = 0, points = 0;
    for (int k = 0; k < 1500; ++k) {
        const Tri t = randomTri(g, 0.05 + 0.5 * rnd(g));
        const D3 p = mul(sphereDir(g), 0.5 + 2.0 * rnd(g));
        const D3 L = sphereDir(g);
        const double w = std::pow(10.0, -3.0 * rnd(g));   // boxes from 1 to 1e-3 wide
        const double u0 = rnd(g) * (1.0 - w), v0 = rnd(g) * (1.0 - w);
        const V2 lo{float(u0), float(v0)}, hi{float(u0 + w), float(v0 + w)};
        const Enclosure e = manifoldEnclose(t, f3(p), f3(L), lo, hi);
        if (e.defined == 0) continue;
        ++boxes;
        for (int s = 0; s < 40; ++s) {
            const V2 uv{lo[0] + float(rnd(g)) * (hi[0] - lo[0]), lo[1] + float(rnd(g)) * (hi[1] - lo[1])};
            const MapResult m = manifoldMap(t, f3(p), f3(L), uv);
            if (e.defined < 0) {
                ASSERT_FALSE(m.ok) << "case " << k << ": the box was certified undefined";
                continue;
            }
            if (!m.ok) continue;   // the float map's own guards (C <= 1e-7)
            ++points;
            auto in = [](float x, V2 iv) { return x >= iv[0] && x <= iv[1]; };
            ASSERT_TRUE(in(m.sigma[0], e.sx) && in(m.sigma[1], e.sy)) << "case " << k << " sigma outside its enclosure";
            ASSERT_TRUE(in(m.du[0], e.dsxdu) && in(m.dv[0], e.dsxdv) && in(m.du[1], e.dsydu) && in(m.dv[1], e.dsydv))
                << "case " << k << " the Jacobian outside its enclosure";
        }
    }
    EXPECT_GT(boxes, 800);
    EXPECT_GT(points, 20000);
}

// Krawczyk's walk (rt/manifold_cert.slang) against a dense double search (a 14 x 14 grid of Newton starts):
// - it never certifies a root the search does not find (no false root, ever);
// - on the showcase's ball (96 x 48, the pass's real case) it certifies every root and leaves no box undecided;
// - on random near triangles (each spanning 30-60 degrees from p, vertex normals turned up to 25 degrees: two roots in
//   one triangle happen there) it certifies every root wherever it leaves no box undecided, and the cases it leaves
//   undecided (each one counted by the pass, never dropped silently) are measured: its interval enclosures are loose
//   on so near and curved a triangle, and 512 boxes do not always reach a decision (0.9% measured, 2026-11-05).
TEST(Manifold, t9_certify_complete) {
    const double kTiltTan = 0.4663;   // tan 25 degrees
    std::mt19937 g(92u);
    int roots = 0, twos = 0, boxes = 0, cases = 0, undecidedCases = 0;
    int ballCases = 0, ballBoxes = 0, ballUndecided = 0, ballMax = 0;
    auto check = [&](const Tri& t, const D3& p, const D3& L, const double target[2], bool ball, int k) {
        const Roots ref = referenceRoots(t, p, L, target, 1e-4, 14);
        const CertResult c = manifoldCertify(t, f3(p), f3(L), {float(target[0]), float(target[1])});
        const char* what = ball ? "ball" : "random";
        boxes += c.boxes;
        ++cases;
        undecidedCases += c.undecided > 0 ? 1 : 0;
        if (ball) {
            ++ballCases;
            ballBoxes += c.boxes;
            ballUndecided += c.undecided;
            ballMax = std::max(ballMax, c.boxes);
        }
        // No false root: every certified root is one the search finds (inside or within the edge margin).
        for (int i = 0; i < c.roots; ++i) {
            bool found = false;
            for (const auto* list : {&ref.inside, &ref.nearEdge})
                for (const auto& q : *list)
                    found = found || (std::abs(c.uv[i][0] - q[0]) < 1e-3 && std::abs(c.uv[i][1] - q[1]) < 1e-3);
            ASSERT_TRUE(found) << what << " case " << k << ": certified a root no search finds at (" << c.uv[i][0]
                               << ", " << c.uv[i][1] << ")";
        }
        // Every root the search finds well inside: certified, wherever the walk decided every box.
        if (c.undecided == 0) {
            for (const auto& q : ref.inside) {
                bool found = false;
                for (int i = 0; i < c.roots; ++i)
                    found = found || (std::abs(c.uv[i][0] - q[0]) < 1e-3 && std::abs(c.uv[i][1] - q[1]) < 1e-3);
                ASSERT_TRUE(found) << what << " case " << k << ": a root at (" << q[0] << ", " << q[1]
                                   << ") not certified, and no box left undecided";
            }
        }
        roots += int(ref.inside.size());
        twos += ref.inside.size() >= 2 ? 1 : 0;
    };
    int randomCases = 0;
    for (int k = 0; k < 4000; ++k) {
        const D3 p0 = mul(sphereDir(g), 0.2 * rnd(g));
        const D3 p1 = add(p0, mul(sphereDir(g), 0.3 + 0.3 * rnd(g)));
        const D3 p2 = add(p0, mul(sphereDir(g), 0.3 + 0.3 * rnd(g)));
        const D3 f = norm(cross(sub(p1, p0), sub(p2, p0)));
        auto tilt = [&] {
            const D3 d = sphereDir(g);
            return norm(add(f, mul(norm(sub(d, mul(f, dot(d, f)))), kTiltTan * rnd(g))));
        };
        const Tri t = makeTri(p0, p1, p2, tilt(), tilt(), tilt());
        const D3 p = add(mul(f, 0.5 + rnd(g)), mul(sphereDir(g), 0.5 * rnd(g)));
        const D3 L = norm(add(f, mul(sphereDir(g), 0.8 * rnd(g))));
        // Aim the slope at the map's value somewhere on the triangle, so most cases hold roots.
        double s0[2];
        if (!sigmaD(t, p, L, rnd(g) * 0.5, rnd(g) * 0.5, s0)) continue;
        const double target[2] = {s0[0] + 0.05 * (rnd(g) - 0.5), s0[1] + 0.05 * (rnd(g) - 0.5)};
        ++randomCases;
        check(t, p, L, target, false, k);
    }
    const int randomUndecided = undecidedCases;
    const std::vector<Tri> ball = uvSphere(0.5, 96, 48);
    for (int k = 0; k < 40; ++k) {
        const D3 p = mul(sphereDir(g), 1.0 + 2.0 * rnd(g));
        const D3 L = sphereDir(g);
        const double r = 0.1 * std::sqrt(rnd(g)), phi = 2.0 * kPi * rnd(g);
        const double target[2] = {r * std::cos(phi), r * std::sin(phi)};
        for (const Tri& t : ball) {
            // Only the triangles that can hold a root (2a's test): the walk on the others is the pass's cost, not
            // its correctness.
            if (!manifoldTriangleTest(t, f3(p), f3(L), {float(target[0]), float(target[1])})) continue;
            check(t, p, L, target, true, k);
        }
    }
    std::printf("t9_certify_complete: %d cases, %d roots, %d triangles with two or more, %d boxes\n", cases, roots,
                twos, boxes);
    std::printf("  the ball: %d candidate triangles, %.1f boxes each (at most %d), %d undecided\n", ballCases,
                ballCases ? double(ballBoxes) / ballCases : 0.0, ballMax, ballUndecided);
    std::printf("  random near triangles: %d of %d cases left a box undecided\n", randomUndecided, randomCases);
    EXPECT_GT(roots, 2000);
    EXPECT_GT(twos, 5);
    EXPECT_EQ(ballUndecided, 0);
    EXPECT_LE(randomUndecided, randomCases / 100);
}

// ---- step 2b T2: two refractions through a solid (manifoldCertifyPair) ------------------------------------

namespace {

// The module's frame at (u, v) in double: the interpolated normal as authored, t from c, b = n x t.
void frameD(const Tri& t, double u, double v, D3& n, D3& tt, D3& b) {
    n = norm(add(d3(t.n0), add(mul(d3(t.a1), u), mul(d3(t.a2), v))));
    const D3 c = d3(t.c);
    tt = norm(sub(c, mul(n, dot(c, n))));
    b = cross(n, tt);
}

// I refracted through m (I . m < 0) with r = eta_from / eta_to; false at total internal reflection.
bool refractD(const D3& I, const D3& m, double r, D3& out) {
    const double ci = -dot(I, m);
    const double k = 1.0 - r * r * (1.0 - ci * ci);
    if (k < 0.0) return false;
    out = norm(add(mul(I, r), mul(m, r * ci - std::sqrt(k))));
    return true;
}

// The nearest triangle a ray from o along dir meets past 1e-6 (Moller-Trumbore in double); -1 for none.
int hitD(const std::vector<Tri>& tris, const D3& o, const D3& dir, double& u, double& v, double& tHit) {
    int best = -1;
    tHit = 1e30;
    for (size_t i = 0; i < tris.size(); ++i) {
        const D3 e1 = d3(tris[i].e1), e2 = d3(tris[i].e2);
        const D3 pv = cross(dir, e2);
        const double det = dot(e1, pv);
        if (std::abs(det) < 1e-14) continue;
        const D3 tv = sub(o, d3(tris[i].p0));
        const double a = dot(tv, pv) / det;
        if (a < 0.0 || a > 1.0) continue;
        const D3 qv = cross(tv, e1);
        const double b = dot(dir, qv) / det;
        if (b < 0.0 || a + b > 1.0) continue;
        const double t = dot(e2, qv) / det;
        if (t > 1e-6 && t < tHit) {
            tHit = t;
            best = int(i);
            u = a;
            v = b;
        }
    }
    return best;
}

// F's residual in double at x (the module's constraint, its slopes recovered): max |slope - target|; huge when undefined.
double pairResidualD(const Tri& t1, const Tri& t2, const D3& X, const D3& L, double eta, const double s[4],
                     const std::array<float, 4>& x) {
    const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), x[0]), mul(d3(t1.e2), x[1])));
    const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), x[2]), mul(d3(t2.e2), x[3])));
    const D3 d = norm(sub(y2, y1)), w = norm(sub(X, y2));
    const D3 h1 = mul(add(L, mul(d, eta)), -1.0), h2 = sub(mul(d, eta), w);
    D3 n1, ta, ba, n2, tb, bb;
    frameD(t1, x[0], x[1], n1, ta, ba);
    frameD(t2, x[2], x[3], n2, tb, bb);
    const double c1 = dot(h1, n1), c2 = dot(h2, n2);
    if (!(c1 > 0.0) || !(c2 > 0.0)) return 1e30;
    return std::max({std::abs(dot(h1, ta) / c1 - s[0]), std::abs(dot(h1, ba) / c1 - s[1]),
                     std::abs(dot(h2, tb) / c2 - s[2]), std::abs(dot(h2, bb) / c2 - s[3])});
}

// A path built by hand: the sun's ray refracted into the ball at (u, v) of triangle i1 through a microfacet normal of
// slope s[0..1], the mesh met in double at (u2, v2) of i2, refracted out through slope s[2..3], X on the way out.
struct BuiltPath {
    size_t i1{0}, i2{0};
    double u{0}, v{0}, u2{0}, v2{0};
    D3 X{}, L{};
    double s[4]{};
};

// Up to `count` paths through `ball` (IOR eta), from attempts drawn by `g`.
std::vector<BuiltPath> buildPaths(const std::vector<Tri>& ball, double eta, std::mt19937& g, int count,
                                  double maxSlope = 0.1, const double* fixedS = nullptr, const D3* fixedL = nullptr) {
    std::vector<BuiltPath> paths;
    for (int k = 0; k < 40 * count && int(paths.size()) < count; ++k) {
        BuiltPath b;
        b.L = fixedL ? *fixedL : norm(add(D3{0, 1, 0}, mul(sphereDir(g), 0.6 * rnd(g))));
        b.i1 = size_t(rnd(g) * double(ball.size())) % ball.size();
        const Tri& t1 = ball[b.i1];
        const double u1 = rnd(g), v1r = rnd(g);
        b.u = u1 * (1.0 - std::sqrt(v1r));
        b.v = std::sqrt(v1r) * rnd(g) * (1.0 - u1);
        if (b.u + b.v > 1.0) continue;
        D3 n1, ta, ba;
        frameD(t1, b.u, b.v, n1, ta, ba);
        if (dot(n1, b.L) < 0.2) continue;   // lit
        auto slope = [&] {
            const double r = maxSlope * std::sqrt(rnd(g)), phi = 2.0 * kPi * rnd(g);
            return std::array<double, 2>{r * std::cos(phi), r * std::sin(phi)};
        };
        auto a1 = slope(), a2 = slope();
        if (fixedS) {
            a1 = {fixedS[0], fixedS[1]};
            a2 = {fixedS[2], fixedS[3]};
        }
        const D3 m1 = norm(add(n1, add(mul(ta, a1[0]), mul(ba, a1[1]))));
        if (dot(m1, b.L) <= 0.0) continue;
        const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), b.u), mul(d3(t1.e2), b.v)));
        D3 d;
        if (!refractD(mul(b.L, -1.0), m1, 1.0 / eta, d)) continue;
        double th;
        const int i2 = hitD(ball, y1, d, b.u2, b.v2, th);
        if (i2 < 0) continue;
        b.i2 = size_t(i2);
        const Tri& t2 = ball[b.i2];
        D3 n2, tb, bb;
        frameD(t2, b.u2, b.v2, n2, tb, bb);
        const D3 m2 = norm(add(n2, add(mul(tb, a2[0]), mul(bb, a2[1]))));
        if (dot(d, m2) <= 0.0) continue;
        D3 w;
        if (!refractD(d, mul(m2, -1.0), eta, w)) continue;   // total internal reflection: no path out
        const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), b.u2), mul(d3(t2.e2), b.v2)));
        b.X = add(y2, mul(w, 0.5 + 1.5 * rnd(g)));
        b.s[0] = a1[0];
        b.s[1] = a1[1];
        b.s[2] = a2[0];
        b.s[3] = a2[1];
        paths.push_back(b);
    }
    return paths;
}

} // namespace

// Paths built by hand on the showcase's ball (96 x 48, IOR 1.5): a point on a lit triangle, two slopes in the glass's
// core (|s| < 0.1), the sun's ray refracted in through the first microfacet normal, the mesh met in double, refracted
// out through the second, and X placed on the way out. Every root the pair walk reports satisfies the constraint in
// double (no false path, ever), it certifies the built path wherever it leaves no box undecided, and the cases it
// leaves undecided (counted by the pass) are measured: 1 of 120, a grazing entry (cos 0.26), 2026-11-05.
TEST(Manifold, t10_pair_known_paths) {
    const std::vector<Tri> ball = uvSphere(0.5, 96, 48);
    const double eta = 1.5;
    std::mt19937 g(101u);
    const std::vector<BuiltPath> paths = buildPaths(ball, eta, g, 120);
    int cases = 0, found = 0, extra = 0, undecided = 0, undecidedCases = 0, boxes = 0, maxBoxes = 0;
    for (const BuiltPath& b : paths) {
        const Tri &t1 = ball[b.i1], &t2 = ball[b.i2];
        // The construction itself meets the constraint (the half vectors are the microfacet normals).
        ASSERT_LT(pairResidualD(t1, t2, b.X, b.L, eta, b.s, {float(b.u), float(b.v), float(b.u2), float(b.v2)}), 1e-4)
            << "case " << cases;
        const PairResult r = manifoldCertifyPair(t1, t2, f3(b.X), f3(b.L), float(eta), {float(b.s[0]), float(b.s[1])},
                                                 {float(b.s[2]), float(b.s[3])});
        undecided += r.undecided;
        boxes += r.boxes;
        maxBoxes = std::max(maxBoxes, r.boxes);
        bool hit = false;
        for (int i = 0; i < r.roots; ++i) {
            const auto& x = r.x[i];
            const bool same = std::abs(x[0] - b.u) < 1e-3 && std::abs(x[1] - b.v) < 1e-3 &&
                              std::abs(x[2] - b.u2) < 1e-3 && std::abs(x[3] - b.v2) < 1e-3;
            hit = hit || same;
            if (!same) ++extra;
            EXPECT_LT(pairResidualD(t1, t2, b.X, b.L, eta, b.s, x), 2e-3) << "case " << cases << ": a false path";
        }
        if (hit) ++found;
        undecidedCases += r.undecided > 0 ? 1 : 0;
        EXPECT_TRUE(hit || r.undecided > 0) << "case " << cases << ": the built path not certified, nothing undecided";
        ++cases;
    }
    std::printf("t10_pair_known_paths: %d paths, %d certified, %d other roots, %d boxes (%.1f each, at most %d), "
                "%d undecided\n",
                cases, found, extra, boxes, cases ? double(boxes) / cases : 0.0, maxBoxes, undecided);
    EXPECT_GT(cases, 80);
    EXPECT_EQ(found + undecidedCases, cases);
    EXPECT_LE(undecidedCases, cases / 50);
}



// The seeded walk (ruling M12): from each built path's X, a seed line crosses the ball (the shadow ray toward the sun,
// MNEE's, else the line toward the ball's centre), Newton walks over the mesh, and the final pair is certified.
// Measured on the showcase's ball, for smooth glass (slopes to 1e-3) and slopes to 0.1: how often each seed lands on
// the built path, on another certified path, or nowhere, and its steps. Every path it reports is certified.
TEST(Manifold, t14_walk) {
    const std::vector<Tri> ball = uvSphere(0.5, 96, 48);
    const double eta = 1.5;
    for (const double maxSlope : {1e-3, 0.1}) {
        std::mt19937 g(141u);
        const std::vector<BuiltPath> paths = buildPaths(ball, eta, g, 60, maxSlope);
        int byShadow = 0, byCentre = 0, other = 0, none = 0, steps = 0, walks = 0, undecided = 0;
        for (const BuiltPath& b : paths) {
            const V2 s1{float(b.s[0]), float(b.s[1])}, s2{float(b.s[2]), float(b.s[3])};
            bool done = false;
            for (int seed = 0; seed < 2 && !done; ++seed) {
                const V3 dir = seed == 0 ? f3(b.L) : f3(norm(mul(b.X, -1.0)));   // the ball is centred at the origin
                const WalkResult r = manifoldWalkPair(ball, f3(b.X), f3(b.L), float(eta), s1, s2, dir);
                undecided += r.undecided;
                steps += r.steps;
                walks += r.steps > 0 ? 1 : 0;
                if (!r.found) continue;
                const Tri &t1 = ball[size_t(r.entry)], &t2 = ball[size_t(r.exit)];
                EXPECT_LT(pairResidualD(t1, t2, b.X, b.L, eta, b.s, r.x), 2e-3) << "a false path";
                const bool isBuilt = size_t(r.entry) == b.i1 && size_t(r.exit) == b.i2 &&
                                     std::abs(r.x[0] - b.u) < 1e-3 && std::abs(r.x[1] - b.v) < 1e-3 &&
                                     std::abs(r.x[2] - b.u2) < 1e-3 && std::abs(r.x[3] - b.v2) < 1e-3;
                if (isBuilt) {
                    (seed == 0 ? byShadow : byCentre) += 1;
                    done = true;
                } else {
                    ++other;
                }
            }
            if (!done) ++none;
        }
        std::printf("t14_walk (slopes to %g): %zu paths; the built path found from the shadow ray %d, from the centre "
                    "line %d, not found %d; other certified paths %d; %.1f steps a walk; %d undecided\n",
                    maxSlope, paths.size(), byShadow, byCentre, none, other, walks ? double(steps) / walks : 0.0,
                    undecided);
        EXPECT_GT(paths.size(), 40u);
    }
}

// ---- step 2b: how much light a found path brings (the estimator) ------------------------------------------

namespace {

// GGX (isotropic) in a frame's slopes: P22(s) = 1 / (pi a^2 (1 + |s|^2 / a^2)^2); D(m) = P22(s) / cos^4 of m.
double ggxP22(double sx, double sy, double a) {
    const double q = 1.0 + (sx * sx + sy * sy) / (a * a);
    return 1.0 / (kPi * a * a * q * q);
}
double ggxD(const D3& m, const D3& n, double a) {
    const double c = dot(m, n);
    if (c <= 0.0) return 0.0;
    const double c2 = c * c, t2 = (1.0 - c2) / c2;
    const double q = 1.0 + t2 / (a * a);
    return 1.0 / (kPi * a * a * c2 * c2 * q * q);
}
double smithG1(const D3& v, const D3& m, const D3& n, double a) {
    const double vn = dot(v, n);
    if (dot(v, m) * vn <= 0.0) return 0.0;
    const double c2 = vn * vn, t2 = (1.0 - c2) / c2;
    return 2.0 / (1.0 + std::sqrt(1.0 + a * a * t2));
}
double fresnelD(double cosI, double eta) {   // eta = transmitted over incident
    const double s2 = (1.0 - cosI * cosI) / (eta * eta);
    if (s2 >= 1.0) return 1.0;
    const double ct = std::sqrt(1.0 - s2);
    const double rs = (cosI - eta * ct) / (cosI + eta * ct), rp = (eta * cosI - ct) / (eta * cosI + ct);
    return 0.5 * (rs * rs + rp * rp);
}

// Walter et al. 2007's BTDF, i on the side of index etaI, o on etaO's (both pointing away from the surface).
double btdfWalter(const D3& i, const D3& o, const D3& n, double etaI, double etaO, double a) {
    D3 m = mul(add(mul(i, etaI), mul(o, etaO)), -1.0);
    if (len(m) <= 0.0) return 0.0;
    m = norm(m);
    if (dot(m, n) < 0.0) m = mul(m, -1.0);
    const double im = dot(i, m), om = dot(o, m), in = dot(i, n), on = dot(o, n);
    const double den = etaI * im + etaO * om;
    if (den * den <= 0.0 || in == 0.0 || on == 0.0) return 0.0;
    const double F = fresnelD(std::abs(im), etaO / etaI);
    const double G = smithG1(i, m, n, a) * smithG1(o, m, n, a);
    return std::abs(im) * std::abs(om) / (std::abs(in) * std::abs(on)) * etaO * etaO * (1.0 - F) * ggxD(m, n, a) * G /
           (den * den);
}

// The shading normal of a triangle at (u, v).
D3 shadeN(const Tri& t, double u, double v) { return norm(add(d3(t.n0), add(mul(d3(t.a1), u), mul(d3(t.a2), v)))); }

// The nearest hit of a ray on a uvSphere(r, sectors, stacks) ball centred at the origin: the analytic sphere's hit names
// the cell, and the triangles of the 3 x 3 cells round it are tested exactly.
int ballHit(const std::vector<Tri>& ball, int sectors, int stacks, double r, const D3& o, const D3& d, bool inside,
            double& u, double& v, double& tHit) {
    const double b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
    if (disc < 0.0) return -1;
    const double tS = inside ? -b + std::sqrt(disc) : -b - std::sqrt(disc);
    const D3 p = add(o, mul(d, tS));
    const double th = std::acos(std::clamp(p[1] / r, -1.0, 1.0));
    double ph = std::atan2(p[2], p[0]);
    if (ph < 0.0) ph += 2.0 * kPi;
    const int j = std::min(stacks - 1, int(th / kPi * stacks)), i = std::min(sectors - 1, int(ph / (2.0 * kPi) * sectors));
    // uvSphere's order: stack by stack, sector by sector; the first and last stacks hold one triangle a sector.
    auto firstOf = [&](int jj) { return jj == 0 ? 0 : sectors + (jj - 1) * 2 * sectors; };
    auto per = [&](int jj) { return (jj == 0 || jj == stacks - 1) ? 1 : 2; };
    int best = -1;
    tHit = 1e30;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            const int jj = j + dj, ii = (i + di + sectors) % sectors;
            if (jj < 0 || jj >= stacks) continue;
            for (int k = 0; k < per(jj); ++k) {
                const int idx = firstOf(jj) + ii * per(jj) + k;
                const Tri& t = ball[size_t(idx)];
                const D3 e1 = d3(t.e1), e2 = d3(t.e2), pv = cross(d, e2);
                const double det = dot(e1, pv);
                if (std::abs(det) < 1e-15) continue;
                const D3 tv = sub(o, d3(t.p0));
                const double a = dot(tv, pv) / det;
                if (a < 0.0 || a > 1.0) continue;
                const D3 qv = cross(tv, e1);
                const double bb = dot(d, qv) / det;
                if (bb < 0.0 || a + bb > 1.0) continue;
                const double tt = dot(e2, qv) / det;
                if (tt > 1e-9 && tt < tHit) {
                    tHit = tt;
                    best = idx;
                    u = a;
                    v = bb;
                }
            }
        }
    return best;
}

// A GGX microfacet normal drawn in proportion to D(m) (m . n): its slope's radius a sqrt(u / (1 - u)).
D3 sampleMicro(const D3& n, double a, std::mt19937& g) {
    const double u = std::min(rnd(g), 1.0 - 1e-12), phi = 2.0 * kPi * rnd(g);
    const double r = a * std::sqrt(u / (1.0 - u));
    const D3 t = perpendicular(n), b = cross(n, t);
    return norm(add(n, add(mul(t, r * std::cos(phi)), mul(b, r * std::sin(phi)))));
}

} // namespace

// The sun through a rough glass ball (96 x 48, radius 0.5, IOR 1.5, GGX roughness 0.1 at both faces) onto a floor 1.5
// below its centre, refracting in and out (TT paths only). The estimator: slopes drawn from GGX's slope density, a walk
// from a seed drawn uniformly in the cone the ball subtends from X, the path it finds weighed by Walter's BTDFs, the
// geometry and 1 / (|det ds/dA| p(s1) p(s2)), and counted once per fresh seed it takes to find that path again
// (Zeltner et al. 2020: on average 1 / its probability, so every reachable path counts exactly). The reference: light
// traced from the sun in double (millions of rays, each microfacet normal drawn by D(m) (m.n) and weighed by Walter's
// sampling weight, counted in a disc round the point). Measured 2026-11-05 with 1,500 draws: +1.7%, -0.05%, -1.8% at the
// three points, 1.1-1.3 walks to find a path again; with the two fixed seeds instead (the shadow ray, the centre line),
// the point under the centre read 18% dark (paths missed on the caustic's axis).
TEST(Manifold, t15_estimator_brightness) {
    const int sectors = 96, stacks = 48;
    const double R = 0.5, eta = 1.5, rough = 0.1, a = rough * rough, floorY = -1.5, E = 1.0;
    const std::vector<Tri> ball = uvSphere(R, sectors, stacks);
    const D3 L{0.0, 1.0, 0.0};   // the sun overhead
    const D3 nX{0.0, 1.0, 0.0};
    const double xs[3] = {0.0, 0.12, 0.25};
    const double rho = 0.02;   // the reference's disc
    // The reference: sun rays uniformly over the ball's shadow disc (area pi R^2), each carrying flux E pi R^2 / N.
    std::mt19937 g(151u);
    const long N = 3000000;
    double hits[3] = {0, 0, 0};
    for (long k = 0; k < N; ++k) {
        const double rr = R * std::sqrt(rnd(g)), ph = 2.0 * kPi * rnd(g);
        const D3 o{rr * std::cos(ph), 2.0, rr * std::sin(ph)};
        const D3 dir{0.0, -1.0, 0.0};
        double u, v, th;
        const int t1 = ballHit(ball, sectors, stacks, R, o, dir, false, u, v, th);
        if (t1 < 0) continue;
        const D3 y1 = add(o, mul(dir, th));
        const D3 n1 = shadeN(ball[size_t(t1)], u, v);
        const D3 m1 = sampleMicro(n1, a, g);
        const D3 i1 = mul(dir, -1.0);
        D3 d;
        if (dot(i1, m1) <= 0.0 || !refractD(dir, m1, 1.0 / eta, d)) continue;
        if (dot(d, n1) >= 0.0) continue;
        // Walter's weight for a normal drawn by D(m) (m.n): |i.m| G (1 - F) / (|i.n| (m.n)).
        double w = (1.0 - fresnelD(dot(i1, m1), eta)) * smithG1(i1, m1, n1, a) * smithG1(mul(d, -1.0), m1, n1, a) *
                   dot(i1, m1) / (std::abs(dot(i1, n1)) * dot(m1, n1));
        const int t2 = ballHit(ball, sectors, stacks, R, y1, d, true, u, v, th);
        if (t2 < 0) continue;
        const D3 y2 = add(y1, mul(d, th));
        const D3 n2 = shadeN(ball[size_t(t2)], u, v);
        const D3 m2 = sampleMicro(n2, a, g);
        const D3 i2 = mul(d, -1.0);
        D3 out;
        if (dot(d, m2) <= 0.0 || !refractD(d, mul(m2, -1.0), eta, out)) continue;
        if (dot(out, n2) <= 0.0) continue;
        w *= (1.0 - fresnelD(dot(d, m2), 1.0 / eta)) * smithG1(i2, mul(m2, -1.0), mul(n2, -1.0), a) *
             smithG1(out, m2, n2, a) * dot(d, m2) / (std::abs(dot(d, n2)) * dot(m2, n2));
        if (out[1] >= 0.0) continue;
        const D3 f = add(y2, mul(out, (floorY - y2[1]) / out[1]));
        for (int q = 0; q < 3; ++q) {
            const double dx = f[0] - xs[q], dz = f[2];
            if (dx * dx + dz * dz <= rho * rho) hits[q] += w;
        }
    }
    // The estimator at each point.
    const int M = 800;
    for (int q = 0; q < 3; ++q) {
        const double ref = hits[q] * E * kPi * R * R / double(N) / (kPi * rho * rho);
        const D3 X{xs[q], floorY, 0.0};
        double sum = 0.0, sumGpu = 0.0;
        int found = 0, foundGpu = 0;
        long totalTrials = 0, trialsGpu = 0;
        for (int k = 0; k < M; ++k) {
            double s[4];
            for (int h = 0; h < 2; ++h) {
                const double u = std::min(rnd(g), 1.0 - 1e-12), ph = 2.0 * kPi * rnd(g);
                const double r = a * std::sqrt(u / (1.0 - u));
                s[2 * h] = r * std::cos(ph);
                s[2 * h + 1] = r * std::sin(ph);
            }
            const V2 s1{float(s[0]), float(s[1])}, s2{float(s[2]), float(s[3])};
            // Zeltner et al. 2020's unbiased estimate: a seed direction drawn uniformly in the cone the ball subtends
            // from X; a path found from it counts once for every fresh seed it takes to find that same path again
            // (on average 1 / its probability), so every path the walk can reach from some seed is counted exactly.
            const D3 axis = norm(mul(X, -1.0));
            const double cosA = std::cos(std::asin(std::min(1.0, 1.02 * R / len(X))));
            const D3 ta = perpendicular(axis), tb = cross(axis, ta);
            auto seedDir = [&] {
                const double c = cosA + (1.0 - cosA) * rnd(g), sn = std::sqrt(std::max(0.0, 1.0 - c * c));
                const double ph = 2.0 * kPi * rnd(g);
                return f3(add(mul(axis, c), add(mul(ta, sn * std::cos(ph)), mul(tb, sn * std::sin(ph)))));
            };
            // The GPU's form (manifoldConnect's manifoldGlass): the walk uncertified, its own slope Jacobian
            // (det dF/dx / (C1 C2)^2), the BSDFs' cosines at the shading normals and the area conversions at the
            // triangles' geometric ones; its re-find trials capped at 64, as the pass caps them.
            {
                const WalkResult r = manifoldWalkPair(ball, f3(X), f3(L), float(eta), s1, s2, seedDir(), false);
                if (r.found && r.slopeDet > 0.0f) {
                    int trials = 1;
                    while (trials < 64) {
                        const WalkResult again =
                            manifoldWalkPair(ball, f3(X), f3(L), float(eta), s1, s2, seedDir(), false);
                        // The same path: the same pair and the same polished root (the walk polishes it in the pair's
                        // own parameters, so two walks to one path end within float rounding of each other).
                        if (again.found && again.entry == r.entry && again.exit == r.exit &&
                            std::abs(again.x[0] - r.x[0]) < 1e-3 && std::abs(again.x[1] - r.x[1]) < 1e-3 &&
                            std::abs(again.x[2] - r.x[2]) < 1e-3 && std::abs(again.x[3] - r.x[3]) < 1e-3)
                            break;
                        ++trials;
                    }
                    trialsGpu += trials;
                    const Tri &t1 = ball[size_t(r.entry)], &t2 = ball[size_t(r.exit)];
                    const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), r.x[0]), mul(d3(t1.e2), r.x[1])));
                    const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), r.x[2]), mul(d3(t2.e2), r.x[3])));
                    const D3 d = norm(sub(y2, y1)), w = norm(sub(X, y2));
                    const double r1 = len(sub(y2, y1)), r2 = len(sub(X, y2));
                    const D3 n1 = shadeN(t1, r.x[0], r.x[1]), n2 = shadeN(t2, r.x[2], r.x[3]);
                    const D3 c1 = cross(d3(t1.e1), d3(t1.e2)), c2 = cross(d3(t2.e1), d3(t2.e2));
                    const double area1 = len(c1), area2 = len(c2);
                    const double eval1 = btdfWalter(L, d, n1, 1.0, eta, a) * std::abs(dot(L, n1));
                    const double eval2 = btdfWalter(mul(d, -1.0), w, n2, eta, 1.0, a) * std::abs(dot(d, n2));
                    const double integrand = E * eval1 * eval2 * std::abs(dot(d, c1)) / area1 / (r1 * r1) *
                                             std::abs(dot(w, c2)) / area2 / (r2 * r2) * std::abs(dot(w, nX));
                    const double detA = double(r.slopeDet) / (area1 * area2);
                    sumGpu += trials * integrand / (detA * ggxP22(s[0], s[1], a) * ggxP22(s[2], s[3], a));
                    ++foundGpu;
                }
            }
            std::vector<std::array<double, 7>> roots;   // (entry, exit, x, weight)
            const WalkResult r = manifoldWalkPair(ball, f3(X), f3(L), float(eta), s1, s2, seedDir());
            if (r.found) {
                int trials = 1;
                while (trials < 1000) {
                    const WalkResult again = manifoldWalkPair(ball, f3(X), f3(L), float(eta), s1, s2, seedDir());
                    if (again.found && again.entry == r.entry && again.exit == r.exit &&
                        std::abs(again.x[0] - r.x[0]) < 1e-4 && std::abs(again.x[1] - r.x[1]) < 1e-4 &&
                        std::abs(again.x[2] - r.x[2]) < 1e-4 && std::abs(again.x[3] - r.x[3]) < 1e-4)
                        break;
                    ++trials;
                }
                totalTrials += trials;
                roots.push_back({double(r.entry), double(r.exit), r.x[0], r.x[1], r.x[2], r.x[3], double(trials)});
            }
            for (const auto& o : roots) {
                const Tri &t1 = ball[size_t(o[0])], &t2 = ball[size_t(o[1])];
                auto slopes = [&](const std::array<double, 4>& x, double out[4]) {
                    const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), x[0]), mul(d3(t1.e2), x[1])));
                    const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), x[2]), mul(d3(t2.e2), x[3])));
                    const D3 d = norm(sub(y2, y1)), w = norm(sub(X, y2));
                    const D3 h1 = mul(add(L, mul(d, eta)), -1.0), h2 = sub(mul(d, eta), w);
                    D3 n1, ta, ba, n2, tb, bb;
                    frameD(t1, x[0], x[1], n1, ta, ba);
                    frameD(t2, x[2], x[3], n2, tb, bb);
                    out[0] = dot(h1, ta) / dot(h1, n1);
                    out[1] = dot(h1, ba) / dot(h1, n1);
                    out[2] = dot(h2, tb) / dot(h2, n2);
                    out[3] = dot(h2, bb) / dot(h2, n2);
                };
                // Polish the float root in double (Newton on the slopes), then its Jacobian by central differences.
                std::array<double, 4> x0{o[2], o[3], o[4], o[5]};
                double J[4][4];
                for (int it = 0; it < 6; ++it) {
                    double f0[4];
                    slopes(x0, f0);
                    for (int i = 0; i < 4; ++i) f0[i] -= s[i];
                    for (int j = 0; j < 4; ++j) {
                        auto xa = x0, xb = x0;
                        xa[j] += 1e-7;
                        xb[j] -= 1e-7;
                        double fa[4], fb[4];
                        slopes(xa, fa);
                        slopes(xb, fb);
                        for (int i = 0; i < 4; ++i) J[i][j] = (fa[i] - fb[i]) / 2e-7;
                    }
                    // Solve J dx = -f (Gauss with pivoting on a copy).
                    double A[4][5];
                    for (int i = 0; i < 4; ++i) {
                        for (int j = 0; j < 4; ++j) A[i][j] = J[i][j];
                        A[i][4] = -f0[i];
                    }
                    for (int c = 0; c < 4; ++c) {
                        int piv = c;
                        for (int r = c + 1; r < 4; ++r)
                            if (std::abs(A[r][c]) > std::abs(A[piv][c])) piv = r;
                        for (int k2 = 0; k2 < 5; ++k2) std::swap(A[c][k2], A[piv][k2]);
                        for (int r = 0; r < 4; ++r) {
                            if (r == c) continue;
                            const double fct = A[r][c] / A[c][c];
                            for (int k2 = c; k2 < 5; ++k2) A[r][k2] -= fct * A[c][k2];
                        }
                    }
                    for (int i = 0; i < 4; ++i) x0[i] += A[i][4] / A[i][i];
                }
                double det = 1.0;
                {
                    double B[4][4];
                    for (int i = 0; i < 4; ++i)
                        for (int j = 0; j < 4; ++j) B[i][j] = J[i][j];
                    for (int c = 0; c < 4; ++c) {
                        int piv = c;
                        for (int r = c + 1; r < 4; ++r)
                            if (std::abs(B[r][c]) > std::abs(B[piv][c])) piv = r;
                        if (piv != c) {
                            for (int k2 = 0; k2 < 4; ++k2) std::swap(B[c][k2], B[piv][k2]);
                            det = -det;
                        }
                        det *= B[c][c];
                        for (int r = c + 1; r < 4; ++r) {
                            const double fct = B[r][c] / B[c][c];
                            for (int k2 = c; k2 < 4; ++k2) B[r][k2] -= fct * B[c][k2];
                        }
                    }
                }
                const double area1 = len(cross(d3(t1.e1), d3(t1.e2))), area2 = len(cross(d3(t2.e1), d3(t2.e2)));
                const double detA = std::abs(det) / (area1 * area2);   // |det ds / dA|
                const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), x0[0]), mul(d3(t1.e2), x0[1])));
                const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), x0[2]), mul(d3(t2.e2), x0[3])));
                const D3 d = norm(sub(y2, y1)), w = norm(sub(X, y2));
                const double r1 = len(sub(y2, y1)), r2 = len(sub(X, y2));
                const D3 n1 = shadeN(t1, x0[0], x0[1]), n2 = shadeN(t2, x0[2], x0[3]);
                const double f1 = btdfWalter(L, d, n1, 1.0, eta, a);
                const double f2 = btdfWalter(mul(d, -1.0), w, n2, eta, 1.0, a);
                const double integrand = E * std::abs(dot(L, n1)) * f1 * std::abs(dot(d, n1)) * std::abs(dot(d, n2)) /
                                         (r1 * r1) * f2 * std::abs(dot(w, n2)) * std::abs(dot(w, nX)) / (r2 * r2);
                sum += o[6] * integrand / (detA * ggxP22(s[0], s[1], a) * ggxP22(s[2], s[3], a));
                ++found;
            }
        }
        const double est = sum / M, estGpu = sumGpu / M;
        std::printf("t15: X at %.2f: the estimator %.4f (%d paths in %d draws, %.1f walks to find each again), the "
                    "traced reference %.4f: %.2f%%; the GPU's form %.4f (%d paths, %.1f walks each): %.2f%%\n",
                    xs[q], est, found, M, found ? double(totalTrials) / found : 0.0, ref, 100.0 * (est - ref) / ref,
                    estGpu, foundGpu, foundGpu ? double(trialsGpu) / foundGpu : 0.0, 100.0 * (estGpu - ref) / ref);
        EXPECT_NEAR(est, ref, 0.05 * ref) << "X at " << xs[q];
        EXPECT_NEAR(estGpu, ref, 0.05 * ref) << "the GPU's form, X at " << xs[q];
    }
}

// ---- step 2c: normal-mapped triangles, cell by cell (CellSlopeMap) ------------------------------------------

namespace {

// Step 2c's map in double, from its description: the cell's (s, t) to the triangle's (u, v), the vertex normal turned
// to p's side, the tangent made perpendicular to it, B = n x T times the sign, the four texels blended bilinearly, the
// mapped normal lifted to 1e-3 above the geometric plane where it dips below, 2a's slope frame (t from c).
struct CellCase {
    MappedTri m;
    Cell cell;
    D3 p, L;
    double o[2], ds[2], dt[2];   // (u, v) = o + ds s + dt t
};

CellCase makeCellCase(const MappedTri& m, const Cell& cell, const D3& p, const D3& L) {
    CellCase c{m, cell, p, L, {}, {}, {}};
    const double a0 = m.uvA[0], a1 = m.uvA[1], b0 = m.uvB[0], b1 = m.uvB[1];
    const double det = a0 * b1 - b0 * a1;
    const double i00 = b1 / det, i01 = -b0 / det, i10 = -a1 / det, i11 = a0 / det;
    const double r0 = double(cell.origin[0]) - m.uv0[0], r1 = double(cell.origin[1]) - m.uv0[1];
    c.o[0] = i00 * r0 + i01 * r1;
    c.o[1] = i10 * r0 + i11 * r1;
    c.ds[0] = i00 * cell.size[0];
    c.ds[1] = i10 * cell.size[0];
    c.dt[0] = i01 * cell.size[1];
    c.dt[1] = i11 * cell.size[1];
    return c;
}

bool cellSigmaD(const CellCase& c, double s, double t, double out[2], double uvOut[2] = nullptr) {
    const Tri& tri = c.m.tri;
    const double u = c.o[0] + c.ds[0] * s + c.dt[0] * t, v = c.o[1] + c.ds[1] * s + c.dt[1] * t;
    if (uvOut) {
        uvOut[0] = u;
        uvOut[1] = v;
    }
    const D3 e1 = d3(tri.e1), e2 = d3(tri.e2);
    const D3 y = add(d3(tri.p0), add(mul(e1, u), mul(e2, v)));
    const D3 H = add(norm(sub(c.p, y)), c.L);
    D3 n = norm(add(d3(tri.n0), add(mul(d3(tri.a1), u), mul(d3(tri.a2), v))));
    const D3 g = cross(e1, e2);
    const double side = dot(g, sub(c.p, d3(tri.p0))) >= 0.0 ? 1.0 : -1.0;
    if (dot(n, g) * side < 0.0) n = mul(n, -1.0);
    const D3 tw = add(d3(c.m.t0), add(mul(d3(c.m.ta1), u), mul(d3(c.m.ta2), v)));
    const D3 T = norm(sub(tw, mul(n, dot(n, tw))));
    const D3 B = mul(cross(n, T), c.m.sign < 0.0f ? -1.0 : 1.0);
    D3 tn{};
    for (int k = 0; k < 3; ++k) {
        const double r0 = c.cell.c00[k] + (double(c.cell.c10[k]) - c.cell.c00[k]) * s;
        const double r1 = c.cell.c01[k] + (double(c.cell.c11[k]) - c.cell.c01[k]) * s;
        tn[k] = r0 + (r1 - r0) * t;
    }
    D3 ns = norm(add(add(mul(T, tn[0]), mul(B, tn[1])), mul(n, tn[2])));
    const D3 geo = mul(norm(g), side);
    if (dot(ns, geo) < 1e-3) ns = norm(add(ns, mul(geo, 1e-3 - dot(ns, geo))));
    const D3 cc = d3(tri.c);
    const D3 tt = norm(sub(cc, mul(ns, dot(cc, ns))));
    const D3 b = cross(ns, tt);
    const double C = dot(H, ns);
    if (!(C > 1e-9)) return false;
    out[0] = dot(H, tt) / C;
    out[1] = dot(H, b) / C;
    return true;
}

bool cellInDomain(const CellCase& c, double s, double t, double margin) {
    const double u = c.o[0] + c.ds[0] * s + c.dt[0] * t, v = c.o[1] + c.ds[1] * s + c.dt[1] * t;
    return std::min({s, t, 1.0 - s, 1.0 - t, u, v, 1.0 - u - v}) >= margin;
}

// Roots of the cell's sigma = target in the cell and on the triangle, by Newton in double from a grid of starts over
// the cell, merged within 1e-6; those within `margin` of the domain's edge (in s, t, u or v) reported apart.
Roots cellReferenceRoots(const CellCase& c, const double target[2], double margin, int grid) {
    Roots r;
    for (int a = 0; a < grid; ++a)
        for (int b = 0; b < grid; ++b) {
            double s = (a + 0.5) / grid, t = (b + 0.5) / grid;
            bool conv = false;
            for (int it = 0; it < 60; ++it) {
                double f[2], fs[2], ft[2];
                if (!cellSigmaD(c, s, t, f)) break;
                const double f0 = f[0] - target[0], f1 = f[1] - target[1];
                if (f0 * f0 + f1 * f1 < 1e-26) {
                    conv = true;
                    break;
                }
                const double h = 1e-7;
                if (!cellSigmaD(c, s + h, t, fs) || !cellSigmaD(c, s, t + h, ft)) break;
                const double j00 = (fs[0] - f[0]) / h, j10 = (fs[1] - f[1]) / h, j01 = (ft[0] - f[0]) / h,
                             j11 = (ft[1] - f[1]) / h;
                const double det = j00 * j11 - j01 * j10;
                if (std::abs(det) < 1e-30) break;
                s -= (j11 * f0 - j01 * f1) / det;
                t -= (-j10 * f0 + j00 * f1) / det;
                if (std::abs(s) > 4 || std::abs(t) > 4) break;
            }
            if (!conv || !cellInDomain(c, s, t, -margin)) continue;
            auto& list = cellInDomain(c, s, t, margin) ? r.inside : r.nearEdge;
            bool dup = false;
            for (auto& q : r.inside) dup = dup || (std::abs(q[0] - s) < 1e-6 && std::abs(q[1] - t) < 1e-6);
            for (auto& q : r.nearEdge) dup = dup || (std::abs(q[0] - s) < 1e-6 && std::abs(q[1] - t) < 1e-6);
            if (!dup) list.push_back({s, t});
        }
    return r;
}

// A random normal-mapped triangle (t9's random triangles: 0.3-0.6 m sides, vertex normals tilted up to 25 degrees;
// tangents in each vertex's plane, turned at random and tilted up to 0.2 out of it; either sign), its texture spanning
// 3-8 texels of a 64 x 64 map a side, and the cell under a random point of it, its texels' normals tilted up to `bump`
// (tan) from the tangent space's z: the bumps a glint comes from. p and L above the triangle; the target the map's
// value at a random point of the cell, jittered.
struct CellDraw {
    CellCase c;
    double target[2];
};
bool randomCell(std::mt19937& g, double bump, CellDraw& out) {
    const double kTiltTan = 0.4663;
    const D3 p0 = mul(sphereDir(g), 0.2 * rnd(g));
    const D3 p1 = add(p0, mul(sphereDir(g), 0.3 + 0.3 * rnd(g)));
    const D3 p2 = add(p0, mul(sphereDir(g), 0.3 + 0.3 * rnd(g)));
    const D3 f = norm(cross(sub(p1, p0), sub(p2, p0)));
    auto tiltOf = [&](const D3& axis, double tanMax) {
        const D3 d = sphereDir(g);
        return norm(add(axis, mul(norm(sub(d, mul(axis, dot(d, axis)))), tanMax * rnd(g))));
    };
    const D3 n0 = tiltOf(f, kTiltTan), n1 = tiltOf(f, kTiltTan), n2 = tiltOf(f, kTiltTan);
    MappedTri m;
    m.tri = makeTri(p0, p1, p2, n0, n1, n2);
    const D3 base = perpendicular(f);
    auto tangentAt = [&](const D3& n) {
        const double a = 2.0 * kPi * rnd(g);
        const D3 b2 = cross(n, base);
        const D3 tdir = add(mul(norm(sub(base, mul(n, dot(base, n)))), std::cos(a)), mul(norm(b2), std::sin(a)));
        return add(tdir, mul(n, 0.4 * (rnd(g) - 0.5)));
    };
    const D3 t0 = tangentAt(n0), t1 = tangentAt(n1), t2 = tangentAt(n2);
    m.t0 = f3(t0);
    m.ta1 = f3(sub(t1, t0));
    m.ta2 = f3(sub(t2, t0));
    m.sign = rnd(g) < 0.5 ? -1.0f : 1.0f;
    const double texel = 1.0 / 64.0;
    const double la = (3.0 + 5.0 * rnd(g)) * texel, lb = (3.0 + 5.0 * rnd(g)) * texel;
    const double aa = 2.0 * kPi * rnd(g), ab = aa + (0.5 + 2.0 * rnd(g));   // not parallel
    m.uv0 = {float(0.2 + 0.6 * rnd(g)), float(0.2 + 0.6 * rnd(g))};
    m.uvA = {float(la * std::cos(aa)), float(la * std::sin(aa))};
    m.uvB = {float(lb * std::cos(ab)), float(lb * std::sin(ab))};
    // The cell under a random point of the triangle.
    double u = rnd(g), v = rnd(g);
    if (u + v > 1.0) {
        u = 1.0 - u;
        v = 1.0 - v;
    }
    const double tu = m.uv0[0] + m.uvA[0] * u + m.uvB[0] * v, tv = m.uv0[1] + m.uvA[1] * u + m.uvB[1] * v;
    const double ci = std::floor(tu / texel - 0.5), cj = std::floor(tv / texel - 0.5);
    Cell cell;
    cell.origin = {float((ci + 0.5) * texel), float((cj + 0.5) * texel)};
    cell.size = {float(texel), float(texel)};
    auto texelNormal = [&] {
        const double r = bump * std::sqrt(rnd(g)), a = 2.0 * kPi * rnd(g);
        return f3(norm(D3{r * std::cos(a), r * std::sin(a), 1.0}));
    };
    cell.c00 = texelNormal();
    cell.c10 = texelNormal();
    cell.c01 = texelNormal();
    cell.c11 = texelNormal();
    const D3 p = add(mul(f, 0.5 + rnd(g)), mul(sphereDir(g), 0.5 * rnd(g)));
    const D3 L = norm(add(f, mul(sphereDir(g), 0.8 * rnd(g))));
    out.c = makeCellCase(m, cell, p, L);
    // The target: the map's value at a random point of the cell on the triangle, jittered.
    for (int k = 0; k < 16; ++k) {
        const double s = rnd(g), t = rnd(g);
        double s0[2];
        if (!cellInDomain(out.c, s, t, 0.0) || !cellSigmaD(out.c, s, t, s0)) continue;
        out.target[0] = s0[0] + 0.02 * (rnd(g) - 0.5);
        out.target[1] = s0[1] + 0.02 * (rnd(g) - 0.5);
        return true;
    }
    return false;
}

} // namespace

// The kernel's cell map is the double map, and its Jacobian the double map's central differences.
TEST(Manifold, m10_cell_map) {
    std::mt19937 g(1010u);
    int checked = 0;
    double worstS = 0.0, worstJ = 0.0;
    for (int k = 0; k < 2000; ++k) {
        CellDraw d;
        if (!randomCell(g, 0.5, d)) continue;
        const double s = rnd(g), t = rnd(g);
        double ref[2], uv[2];
        if (!cellSigmaD(d.c, s, t, ref, uv)) continue;
        const CellMapResult r = manifoldCellMap(d.c.m, d.c.cell, f3(d.c.p), f3(d.c.L), {float(s), float(t)});
        if (!r.ok) continue;
        const double scale = std::max(1.0, std::max(std::abs(ref[0]), std::abs(ref[1])));
        worstS = std::max(worstS, std::max(std::abs(r.sigma[0] - ref[0]), std::abs(r.sigma[1] - ref[1])) / scale);
        EXPECT_NEAR(r.uv[0], uv[0], 1e-5);
        EXPECT_NEAR(r.uv[1], uv[1], 1e-5);
        const double h = 1e-6;
        double a[2], b[2], c2[2], e[2];
        if (!cellSigmaD(d.c, s + h, t, a) || !cellSigmaD(d.c, s - h, t, b) || !cellSigmaD(d.c, s, t + h, c2) ||
            !cellSigmaD(d.c, s, t - h, e))
            continue;
        const double J[4] = {(a[0] - b[0]) / (2 * h), (c2[0] - e[0]) / (2 * h), (a[1] - b[1]) / (2 * h),
                             (c2[1] - e[1]) / (2 * h)};
        double jScale = 1.0;
        for (double v : J) jScale = std::max(jScale, std::abs(v));
        for (int i = 0; i < 4; ++i) worstJ = std::max(worstJ, std::abs(r.J[i] - J[i]) / jScale);
        ++checked;
    }
    std::printf("m10_cell_map: %d points; sigma within %.2g, its Jacobian within %.2g (relative)\n", checked, worstS,
                worstJ);
    EXPECT_GT(checked, 1500);
    EXPECT_LT(worstS, 1e-4);
    EXPECT_LT(worstJ, 1e-3);
}

// Every root of a normal-mapped cell that a dense double search finds is certified, and nothing else: random
// triangles, tangents, texture coordinates and bumpy cells (step 2c's map, rt/manifold_cert.slang's CellSlopeMap).
TEST(Manifold, m10_cells_complete) {
    std::mt19937 g(1011u);
    int cases = 0, roots = 0, twos = 0, boxes = 0, undecidedCases = 0, maxBoxes = 0;
    for (int k = 0; k < 3000; ++k) {
        CellDraw d;
        if (!randomCell(g, k % 3 == 0 ? 1.0 : 0.5, d)) continue;
        const Roots ref = cellReferenceRoots(d.c, d.target, 1e-4, 16);
        bool defined = false;
        const CertResult c = manifoldCertifyCell(d.c.m, d.c.cell, f3(d.c.p), f3(d.c.L),
                                                 {float(d.target[0]), float(d.target[1])}, &defined);
        ASSERT_TRUE(defined) << "case " << k;
        ++cases;
        boxes += c.boxes;
        maxBoxes = std::max(maxBoxes, c.boxes);
        undecidedCases += c.undecided > 0 ? 1 : 0;
        for (int i = 0; i < c.roots; ++i) {
            bool found = false;
            for (const auto* list : {&ref.inside, &ref.nearEdge})
                for (const auto& q : *list)
                    found = found || (std::abs(c.uv[i][0] - q[0]) < 1e-3 && std::abs(c.uv[i][1] - q[1]) < 1e-3);
            ASSERT_TRUE(found) << "case " << k << ": certified a root no search finds at (" << c.uv[i][0] << ", "
                               << c.uv[i][1] << ")";
        }
        if (c.undecided == 0) {
            for (const auto& q : ref.inside) {
                bool found = false;
                for (int i = 0; i < c.roots; ++i)
                    found = found || (std::abs(c.uv[i][0] - q[0]) < 1e-3 && std::abs(c.uv[i][1] - q[1]) < 1e-3);
                ASSERT_TRUE(found) << "case " << k << ": a root at (" << q[0] << ", " << q[1]
                                   << ") not certified, and no box left undecided";
            }
        }
        roots += int(ref.inside.size());
        twos += ref.inside.size() >= 2 ? 1 : 0;
    }
    std::printf("m10_cells_complete: %d cells, %d roots, %d cells with two or more, %.1f boxes a cell (at most %d), "
                "%d cells left a box undecided\n",
                cases, roots, twos, cases ? double(boxes) / cases : 0.0, maxBoxes, undecidedCases);
    EXPECT_GT(cases, 2500);
    EXPECT_GT(roots, 1500);
    // Sound (asserted above), not yet complete: 44% of cells spend the 512-box budget (79% before the map lost its
    // divisions): its interval enclosures overestimate F and its Jacobian over a box many times over (ruling M11,
    // docs/specs/2026-11-04-caustics-step2-design.md "Step 2c").
}
