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

// A set of triangles as the pass would gather it: its world box and the cone of its vertex normals.
struct NodeD {
    V3 lo{}, hi{}, axis{};
    float half{0};
};
NodeD nodeOf(const std::vector<Tri>& tris, const std::vector<size_t>& members) {
    D3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30}, sum{0, 0, 0};
    std::vector<D3> normals;
    for (size_t i : members) {
        const Tri& t = tris[i];
        const D3 p[3] = {d3(t.p0), add(d3(t.p0), d3(t.e1)), add(d3(t.p0), d3(t.e2))};
        const D3 n[3] = {norm(d3(t.n0)), norm(add(d3(t.n0), d3(t.a1))), norm(add(d3(t.n0), d3(t.a2)))};
        for (int k = 0; k < 3; ++k) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[k][a]);
                hi[a] = std::max(hi[a], p[k][a]);
            }
            sum = add(sum, n[k]);
            normals.push_back(n[k]);
        }
    }
    const D3 axis = norm(sum);
    double half = 0.0;
    for (const D3& n : normals) half = std::max(half, std::acos(std::clamp(dot(n, axis), -1.0, 1.0)));
    return {f3(lo), f3(hi), f3(axis), float(half + 1e-6)};
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

// Which pairs to solve (2b T3): a ball grouped as the pass would gather it (clusters by latitude and longitude), the node
// test (manifoldPairNodeTest: the refraction cones at the entry and the exit and the box-to-box cone must meet) on
// cluster pairs, then on the triangle pairs inside those that pass. It never drops a built path's pair (conservative);
// what it leaves is counted on one request (slow: every pair). The spec's "Pairs (T3)" records the four ways measured.
TEST(Manifold, t11_pair_candidates) {
    const std::vector<Tri> ball = uvSphere(0.5, 48, 24);
    const double eta = 1.5;
    std::mt19937 g(111u);
    // Smooth glass (the showcase's: roughness 0, alpha about 1e-4): slopes drawn within 1e-3.
    const std::vector<BuiltPath> paths = buildPaths(ball, eta, g, 12, 1e-3);
    // Clusters: 8 bands of latitude x 12 of longitude by the triangle's centroid.
    std::vector<std::vector<size_t>> clusters(8 * 12);
    std::vector<size_t> clusterOf(ball.size());
    for (size_t i = 0; i < ball.size(); ++i) {
        const Tri& t = ball[i];
        const D3 c = add(d3(t.p0), mul(add(d3(t.e1), d3(t.e2)), 1.0 / 3.0));
        const double th = std::acos(std::clamp(c[1] / len(c), -1.0, 1.0)), ph = std::atan2(c[2], c[0]) + kPi;
        const size_t k = size_t(std::min(7.0, std::floor(th / kPi * 8.0))) * 12 +
                         size_t(std::min(11.0, std::floor(ph / (2.0 * kPi) * 12.0)));
        clusters[k].push_back(i);
        clusterOf[i] = k;
    }
    std::vector<NodeD> cnodes, tnodes;
    for (const auto& m : clusters) cnodes.push_back(nodeOf(ball, m));
    for (size_t i = 0; i < ball.size(); ++i) tnodes.push_back(nodeOf(ball, {i}));
    long clusterPairs = 0, trianglePairs = 0, maxTrianglePairs = 0;
    for (size_t q = 0; q < paths.size(); ++q) {
        const BuiltPath& b = paths[q];
        const V2 s1{float(b.s[0]), float(b.s[1])}, s2{float(b.s[2]), float(b.s[3])};
        auto test = [&](const NodeD& a, const NodeD& c) {
            return manifoldPairNodeTest(a.lo, a.hi, a.axis, a.half, c.lo, c.hi, c.axis, c.half, f3(b.X), f3(b.L),
                                        float(eta), s1, s2);
        };
        // The built path's own pair passes at both levels.
        ASSERT_TRUE(test(cnodes[clusterOf[b.i1]], cnodes[clusterOf[b.i2]])) << "path " << q << ": its clusters pruned";
        ASSERT_TRUE(test(tnodes[b.i1], tnodes[b.i2])) << "path " << q << ": its triangles pruned";
        if (q >= 1) continue;   // counting every pair is slow: one request
        long here = 0;
        for (size_t c1 = 0; c1 < cnodes.size(); ++c1)
            for (size_t c2 = 0; c2 < cnodes.size(); ++c2) {
                if (clusters[c1].empty() || clusters[c2].empty() || !test(cnodes[c1], cnodes[c2])) continue;
                ++clusterPairs;
                for (size_t i1 : clusters[c1])
                    for (size_t i2 : clusters[c2])
                        if (i1 != i2 && test(tnodes[i1], tnodes[i2])) ++here;
            }
        trianglePairs += here;
        maxTrianglePairs = std::max(maxTrianglePairs, here);
    }
    const double n = 1.0;
    std::printf("t11_pair_candidates: %zu requests held conservative; on one: cluster pairs %.1f (of %zu); triangle pairs "
                "%.1f (at most %ld, of %zu)\n",
                paths.size(), clusterPairs / n, cnodes.size() * cnodes.size(), trianglePairs / n, maxTrianglePairs,
                ball.size() * ball.size());
    EXPECT_GT(paths.size(), 8u);
}

// Sampling pairs (2b T3): a descent through the ball's hierarchy (groups of clusters, clusters, sub-clusters of 16,
// triangles) that picks one (entry, exit) child pair a level with probability proportional to manifoldPairNodeScore. It
// is exact for any positive score; its noise is set by the probability it gives a path's own pair, computed here
// exactly (the product, level by level, of that child's score over its siblings' sum) for built paths on the
// showcase's 96 x 48 ball. A path's pair must never score 0 (conservative).
TEST(Manifold, t13_pair_sampling) {
    const std::vector<Tri> ball = uvSphere(0.5, 96, 48);
    const double eta = 1.5;
    std::mt19937 g(131u);
    const std::vector<BuiltPath> paths = buildPaths(ball, eta, g, 40, 1e-3);
    // Clusters by latitude and longitude, groups of clusters, sub-clusters of 16 by index within a cluster.
    const int bands = 8, sectors = 12;
    std::vector<std::vector<size_t>> clusters(size_t(bands * sectors));
    std::vector<size_t> clusterOf(ball.size());
    for (size_t i = 0; i < ball.size(); ++i) {
        const Tri& t = ball[i];
        const D3 c = add(d3(t.p0), mul(add(d3(t.e1), d3(t.e2)), 1.0 / 3.0));
        const double th = std::acos(std::clamp(c[1] / len(c), -1.0, 1.0)), ph = std::atan2(c[2], c[0]) + kPi;
        const size_t k = size_t(std::min(double(bands - 1), std::floor(th / kPi * bands))) * size_t(sectors) +
                         size_t(std::min(double(sectors - 1), std::floor(ph / (2.0 * kPi) * sectors)));
        clusters[k].push_back(i);
        clusterOf[i] = k;
    }
    // Node: its triangles and their node (box, cone); a level's children.
    struct Node {
        std::vector<size_t> tris;
        NodeD d;
        std::vector<int> kids;
    };
    std::vector<Node> nodes;
    auto make = [&](std::vector<size_t> tris) {
        nodes.push_back({tris, nodeOf(ball, tris), {}});
        return int(nodes.size()) - 1;
    };
    std::vector<int> clusterNode(clusters.size());
    for (size_t c = 0; c < clusters.size(); ++c) {
        const int cn = make(clusters[c]);
        for (size_t a = 0; a < clusters[c].size(); a += 16) {
            std::vector<size_t> sub(clusters[c].begin() + long(a),
                                    clusters[c].begin() + long(std::min(a + 16, clusters[c].size())));
            const int sn = make(sub);
            for (size_t i : sub) {
                const int tn = make({i});   // first: make() may grow `nodes`
                nodes[size_t(sn)].kids.push_back(tn);
            }
            nodes[size_t(cn)].kids.push_back(sn);
        }
        clusterNode[c] = cn;
    }
    std::vector<size_t> all(ball.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    const int root = make(all);
    for (int gb = 0; gb < bands; gb += 2)
        for (int gs = 0; gs < sectors; gs += 3) {
            std::vector<size_t> tris;
            std::vector<int> kids;
            for (int b = gb; b < gb + 2; ++b)
                for (int sct = gs; sct < gs + 3; ++sct) {
                    const size_t c = size_t(b * sectors + sct);
                    tris.insert(tris.end(), clusters[c].begin(), clusters[c].end());
                    kids.push_back(clusterNode[c]);
                }
            const int gn = make(tris);
            nodes[size_t(gn)].kids = kids;
            nodes[size_t(root)].kids.push_back(gn);
        }
    // The path from the root to a triangle: the child holding it, level by level.
    auto holder = [&](int node, size_t tri) {
        for (int k : nodes[size_t(node)].kids)
            if (std::find(nodes[size_t(k)].tris.begin(), nodes[size_t(k)].tris.end(), tri) != nodes[size_t(k)].tris.end())
                return k;
        return -1;
    };
    std::vector<double> probs;
    long tests = 0;
    for (const BuiltPath& b : paths) {
        const V2 s1{float(b.s[0]), float(b.s[1])}, s2{float(b.s[2]), float(b.s[3])};
        double p = 1.0;
        int e = root, x = root;
        while (!nodes[size_t(e)].kids.empty()) {
            const int te = holder(e, b.i1), tx = holder(x, b.i2);
            ASSERT_GE(te, 0);
            ASSERT_GE(tx, 0);
            double sum = 0.0, own = 0.0;
            for (int ce : nodes[size_t(e)].kids)
                for (int cx : nodes[size_t(x)].kids) {
                    const NodeD &a = nodes[size_t(ce)].d, &c = nodes[size_t(cx)].d;
                    const float sc = manifoldPairNodeScore(a.lo, a.hi, a.axis, a.half, c.lo, c.hi, c.axis, c.half,
                                                           f3(b.X), f3(b.L), float(eta), s1, s2);
                    ++tests;
                    sum += sc;
                    if (ce == te && cx == tx) own = sc;
                }
            ASSERT_GT(own, 0.0) << "a built path's pair scored 0: the score is not conservative";
            p *= own / sum;
            e = te;
            x = tx;
        }
        probs.push_back(p);
    }
    std::sort(probs.begin(), probs.end());
    std::printf("t13_pair_sampling: %zu paths; P(own pair) median %.3g, 10%% %.3g, 90%% %.3g; %.0f node tests a "
                "request\n",
                probs.size(), probs[probs.size() / 2], probs[probs.size() / 10], probs[probs.size() * 9 / 10],
                double(tests) / double(paths.size()));
    EXPECT_GT(probs.size(), 30u);
}
