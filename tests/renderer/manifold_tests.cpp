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
    int cases = 0, found = 0, extra = 0, undecided = 0, undecidedCases = 0, boxes = 0, maxBoxes = 0;
    for (int k = 0; k < 400 && cases < 120; ++k) {
        const D3 L = norm(add(D3{0, 1, 0}, mul(sphereDir(g), 0.6 * rnd(g))));
        const Tri& t1 = ball[size_t(rnd(g) * double(ball.size())) % ball.size()];
        const double u1 = rnd(g), v1r = rnd(g);
        const double u = u1 * (1.0 - std::sqrt(v1r)), v = std::sqrt(v1r) * rnd(g) * (1.0 - u1);
        if (u + v > 1.0) continue;
        D3 n1, ta, ba;
        frameD(t1, u, v, n1, ta, ba);
        if (dot(n1, L) < 0.2) continue;   // lit
        auto slope = [&] {
            const double r = 0.1 * std::sqrt(rnd(g)), phi = 2.0 * kPi * rnd(g);
            return std::array<double, 2>{r * std::cos(phi), r * std::sin(phi)};
        };
        const auto a1 = slope(), a2 = slope();
        const D3 m1 = norm(add(n1, add(mul(ta, a1[0]), mul(ba, a1[1]))));
        if (dot(m1, L) <= 0.0) continue;
        const D3 y1 = add(d3(t1.p0), add(mul(d3(t1.e1), u), mul(d3(t1.e2), v)));
        D3 d;
        if (!refractD(mul(L, -1.0), m1, 1.0 / eta, d)) continue;
        double u2, v2, th;
        const int i2 = hitD(ball, y1, d, u2, v2, th);
        if (i2 < 0) continue;
        const Tri& t2 = ball[size_t(i2)];
        D3 n2, tb, bb;
        frameD(t2, u2, v2, n2, tb, bb);
        const D3 m2 = norm(add(n2, add(mul(tb, a2[0]), mul(bb, a2[1]))));
        if (dot(d, m2) <= 0.0) continue;
        D3 w;
        if (!refractD(d, mul(m2, -1.0), eta, w)) continue;   // total internal reflection: no path out
        const D3 y2 = add(d3(t2.p0), add(mul(d3(t2.e1), u2), mul(d3(t2.e2), v2)));
        const D3 X = add(y2, mul(w, 0.5 + 1.5 * rnd(g)));
        const double s[4] = {a1[0], a1[1], a2[0], a2[1]};
        // The construction itself meets the constraint (the half vectors are the microfacet normals).
        ASSERT_LT(pairResidualD(t1, t2, X, L, eta, s, {float(u), float(v), float(u2), float(v2)}), 1e-4)
            << "case " << k;
        const PairResult r = manifoldCertifyPair(t1, t2, f3(X), f3(L), float(eta), {float(s[0]), float(s[1])},
                                                 {float(s[2]), float(s[3])});
        ++cases;
        undecided += r.undecided;
        boxes += r.boxes;
        maxBoxes = std::max(maxBoxes, r.boxes);
        bool hit = false;
        for (int i = 0; i < r.roots; ++i) {
            const auto& x = r.x[i];
            const bool same = std::abs(x[0] - u) < 1e-3 && std::abs(x[1] - v) < 1e-3 && std::abs(x[2] - u2) < 1e-3 &&
                              std::abs(x[3] - v2) < 1e-3;
            hit = hit || same;
            if (!same) ++extra;
            EXPECT_LT(pairResidualD(t1, t2, X, L, eta, s, x), 2e-3) << "case " << k << ": a false path";
        }
        if (hit) ++found;
        undecidedCases += r.undecided > 0 ? 1 : 0;
        EXPECT_TRUE(hit || r.undecided > 0) << "case " << k << ": the built path not certified, nothing undecided";
    }
    std::printf("t10_pair_known_paths: %d paths, %d certified, %d other roots, %d boxes (%.1f each, at most %d), "
                "%d undecided\n",
                cases, found, extra, boxes, cases ? double(boxes) / cases : 0.0, maxBoxes, undecided);
    EXPECT_GT(cases, 80);
    EXPECT_EQ(found + undecidedCases, cases);
    EXPECT_LE(undecidedCases, cases / 50);
}
