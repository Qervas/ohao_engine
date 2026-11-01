// OpenPBR's energy tables (shaders/material/openpbr.slang, OpenPBRTables), integrated from the
// module's own code on the CPU and written as ohao/render/material/openpbr_tables.hpp:
//
//   E1, S5, S6  [rough][mu], 32 x 32: GGX single scatter's directional albedo with F = 1, and
//               weighted by (1 - c)^5 and c (1 - c)^6 (c = m.wo): F82's albedo is linear in them
//   E1Avg       [rough], 32: 2 * integral of the interpolated E1(mu) mu dmu
//   FavgD       [eta], 32, eta in [1, 3]: the exact dielectric Fresnel's cosine-weighted average
//   EssD        [eta][rough][mu], 16^3: the dielectric's single-scatter albedo, exact Fresnel
//   EssDLow     the same at eta in [1, 1.3], the eta axis at 1 + 0.3 g^2 (g the grid): a base under
//               a coat sees an IOR near 1 (1.5 under 1.6: 1.067), where grazing Fresnel changes
//               fastest and EssD's 0.13 spacing left a coated white furnace 1.6% over at 85 degrees
//   AnisoRough  [aniso][rough][mu][phi], 8 x 16 x 16 x 8 (aniso at 1 - (1 - g)^2, phi over [0, pi/2]
//               from the tangent): the
//               isotropic roughness whose E1, as the lookups read it, is the anisotropic lobe's
//               single-scatter albedo there (OpenPBR's alpha mapping); every table above is read at
//               it, so an anisotropic lobe is compensated as an isotropic one is
//   AnisoE1Avg  [aniso][rough], 8 x 16: the cosine-weighted average of E1 read at AnisoRough
//   EssT        [n][rough][mu], EssD's mu and roughness, 32 points of n in [1, 3]: the refraction's single-scatter albedo
//               entering a dielectric of relative IOR n, exact Fresnel (1 - F)
//   EssDIn      the same grid: the reflection's leaving it (relative IOR 1 / n; total internal
//               reflection included)
//   EssTIn      the same grid: the refraction's leaving it
//   Ltc         [rough][u][4], 32 x 32, u = sqrt(1 - mu): the linearly transformed cosine fitted to
//               GGX single scatter (F = 1) at alpha = rough^2 (rough held at 0.05 and above), as the
//               normalized inverse matrix (Heitz, Dupuy, Hill & Neubelt 2016): rows (a, 0, b),
//               (0, 1, 0), (c, 0, d) in the frame of the normal and the view's tangent
//   AnisoS56    [aniso][rough][mu][phi][2], AnisoRough's grid: the anisotropic lobe's S5 and S6
//               (single scatter weighted by (1 - c)^5 and c (1 - c)^6), interleaved. With E1 at
//               AnisoRough they make an F82 metal's albedo exact; a dielectric's is corrected by
//               them (OpenPBRBase.viewedFrom)
//
// Grid point i of n is at i / (n - 1), a mu axis at its square; mu is evaluated at no less than 1e-3 and roughness at no
// less than the module's kMinSpecularRoughness (0.01), where the lookups clamp. Each cell is the
// mean over a 2D Hammersley set: the same bytes every run.
//
//   openpbr_tables OUT.hpp            write the header
//   openpbr_tables --check OUT.hpp    exit 1 when OUT.hpp is not what this build writes
#include "openpbr_cpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using ohao::material::Item;
using ohao::material::Op;

constexpr unsigned kMu = 32, kRough = 32, kEta = 32;
constexpr unsigned kDMu = 16, kDRough = 16, kDEta = 16;
constexpr float kEtaMax = 3.0f;
constexpr float kLowEtaSpan = 0.3f;
constexpr unsigned kAA = 8, kAR = 16, kAM = 16, kAP = 8;
constexpr unsigned kGEta = 32;   // the glass tables' IOR points
constexpr unsigned kAnisoSamples = 1u << 11;
constexpr float kMinRough = 0.01f;
constexpr float kMinMu = 1e-3f;
constexpr unsigned kSamples = 1u << 14;
constexpr unsigned kDielSamples = 1u << 12;

float radicalInverse(unsigned i) {
    i = (i << 16u) | (i >> 16u);
    i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
    i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
    i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
    i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
    return static_cast<float>(i) * (1.0f / 4294967296.0f);
}

float gridAt(unsigned i, unsigned n) { return static_cast<float>(i) / static_cast<float>(n - 1); }
// The anisotropy axis at 1 - (1 - g)^2, dense toward 1 where the lobe narrows fastest (the module
// looks it up by 1 - sqrt(1 - a)).
float anisoAt(unsigned i) { return 1.0f - (1.0f - gridAt(i, kAA)) * (1.0f - gridAt(i, kAA)); }
// A mu axis is sampled at the square of the grid (the module looks it up by sqrt(mu)).
float muAt(unsigned i, unsigned n) { return gridAt(i, n) * gridAt(i, n); }

// The mean of the Tables op's four weights at (mu, rough, eta) over n Hammersley points.
std::array<double, 4> integrate(float mu, float rough, float eta, unsigned n) {
    mu = std::max(mu, kMinMu);
    rough = std::max(rough, kMinRough);
    std::vector<Item> items(n);
    const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
    for (unsigned k = 0; k < n; ++k) {
        Item& it = items[k];
        it.op = Op::Tables;
        it.material.specularRoughness = rough;
        it.wo = {s, 0.0f, mu};
        it.v = {(static_cast<float>(k) + 0.5f) / static_cast<float>(n), radicalInverse(k), eta};
    }
    ohao::material::run(items, {});
    std::array<double, 4> sum{};
    for (const Item& it : items)
        for (int c = 0; c < 4; ++c) sum[c] += it.r0[c];
    for (double& v : sum) v /= n;
    return sum;
}

// ---- linearly transformed cosines (Heitz, Dupuy, Hill & Neubelt 2016) ----------------------
// Our own fit, after the paper's: for each roughness and view, the matrix M whose transform of the
// clamped cosine best matches GGX single scatter (F = 1), normalized, by Nelder-Mead on the
// paper's error (the cubed difference, sampled from both distributions). GGX is written here as
// the module writes it (height-correlated G2, the same D).

constexpr unsigned kLtcN = 32;
constexpr double kPi = 3.14159265358979323846;

struct V3 {
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalized(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }

struct M3 {
    double m[3][3];
};
V3 mul(const M3& a, V3 v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
double det(const M3& a) {
    const auto& m = a.m;
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
M3 inverse(const M3& a) {
    const auto& m = a.m;
    const double d = det(a);
    M3 r{};
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
    return r;
}

double ggxD(V3 h, double a2) {
    const double d = h.x * h.x + h.y * h.y + h.z * h.z * a2;
    return a2 / (kPi * d * d);
}
double ggxLambda(double mu, double a2) { return 0.5 * (std::sqrt(1.0 + a2 * (1.0 - mu * mu) / (mu * mu)) - 1.0); }
// f cos(wi), F = 1 (the module's ggxSingle).
double ggxSingle(V3 wo, V3 wi, double a2) {
    if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
    const V3 h = normalized(wo + wi);
    return ggxD(h, a2) / (1.0 + ggxLambda(wo.z, a2) + ggxLambda(wi.z, a2)) / (4.0 * wo.z);
}
// Heitz 2018's visible normals, reflected; and the reflected direction's density.
V3 ggxSample(V3 wo, double alpha, double u1, double u2) {
    const V3 vh = normalized({alpha * wo.x, alpha * wo.y, wo.z});
    const double l2 = vh.x * vh.x + vh.y * vh.y;
    const V3 t1 = l2 > 0.0 ? V3{-vh.y, vh.x, 0.0} * (1.0 / std::sqrt(l2)) : V3{1.0, 0.0, 0.0};
    const V3 t2 = cross(vh, t1);
    const double r = std::sqrt(u1), phi = 2.0 * kPi * u2;
    const double p1 = r * std::cos(phi);
    double p2 = r * std::sin(phi);
    const double sh = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - sh) * std::sqrt(std::max(0.0, 1.0 - p1 * p1)) + sh * p2;
    const V3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0, 1.0 - p1 * p1 - p2 * p2));
    const V3 h = normalized({alpha * nh.x, alpha * nh.y, std::max(0.0, nh.z)});
    return h * (2.0 * dot(wo, h)) + wo * -1.0;
}
double ggxPdf(V3 wo, V3 wi, double a2) {
    if (wi.z <= 0.0) return 0.0;
    const V3 h = normalized(wo + wi);
    return ggxD(h, a2) / (1.0 + ggxLambda(wo.z, a2)) / (4.0 * wo.z);
}

struct Ltc {
    double m11 = 1.0, m22 = 1.0, m13 = 0.0;
    V3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    M3 M{}, inv{};
    double invDet = 1.0;
    void update() {
        const M3 r{{{X.x, Y.x, Z.x}, {X.y, Y.y, Z.y}, {X.z, Y.z, Z.z}}};
        const M3 sc{{{m11, 0.0, m13}, {0.0, m22, 0.0}, {0.0, 0.0, 1.0}}};
        M = mul(r, sc);
        inv = inverse(M);
        invDet = std::abs(det(inv));
    }
    double eval(V3 L) const {
        const V3 o = mul(inv, L);
        const double l = std::sqrt(dot(o, o));
        const double dz = o.z / l;
        return dz > 0.0 ? dz / kPi * invDet / (l * l * l) : 0.0;
    }
    V3 sample(double u1, double u2) const {
        const double r = std::sqrt(u1), phi = 2.0 * kPi * u2;
        return normalized(mul(M, V3{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0, 1.0 - u1))}));
    }
};

// The paper's error: the cubed difference against the normalized lobe, from both distributions.
double ltcError(const Ltc& l, V3 V, double alpha, double norm) {
    constexpr unsigned n = 24;
    const double a2 = alpha * alpha;
    double sum = 0.0;
    for (unsigned i = 0; i < n; ++i)
        for (unsigned j = 0; j < n; ++j) {
            const double u1 = (i + 0.5) / n, u2 = (j + 0.5) / n;
            {
                const V3 L = l.sample(u1, u2);
                const double pl = l.eval(L), fb = ggxSingle(V, L, a2) / norm, pb = ggxPdf(V, L, a2);
                if (pl + pb > 0.0) sum += std::pow(std::abs(fb - pl), 3.0) / (pl + pb);
            }
            {
                const V3 L = ggxSample(V, alpha, u1, u2);
                if (L.z <= 0.0) continue;
                const double pl = l.eval(L), fb = ggxSingle(V, L, a2) / norm, pb = ggxPdf(V, L, a2);
                if (pl + pb > 0.0) sum += std::pow(std::abs(fb - pl), 3.0) / (pl + pb);
            }
        }
    return sum / (2.0 * n * n);
}

// Nelder-Mead over (m11, m22, m13).
void ltcFit(Ltc& l, V3 V, double alpha, double norm) {
    double pts[4][3];
    double val[4];
    const double start[3] = {l.m11, l.m22, l.m13};
    auto f = [&](const double* p) {
        Ltc t = l;
        t.m11 = std::max(p[0], 1e-5);
        t.m22 = std::max(p[1], 1e-5);
        t.m13 = p[2];
        t.update();
        return ltcError(t, V, alpha, norm);
    };
    for (int k = 0; k < 4; ++k) {
        for (int d = 0; d < 3; ++d) pts[k][d] = start[d];
        if (k > 0) pts[k][k - 1] += (k - 1 < 2 ? std::max(0.05, 0.1 * start[k - 1]) : 0.05);
        val[k] = f(pts[k]);
    }
    for (int iter = 0; iter < 120; ++iter) {
        int order[4] = {0, 1, 2, 3};
        std::sort(order, order + 4, [&](int a, int b) { return val[a] < val[b]; });
        const int best = order[0], worst = order[3], second = order[2];
        if (std::abs(val[worst] - val[best]) < 1e-9 * std::max(1.0, val[best])) break;
        double c[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k)
            if (k != worst)
                for (int d = 0; d < 3; ++d) c[d] += pts[k][d] / 3.0;
        double r[3], e[3], q[3];
        for (int d = 0; d < 3; ++d) r[d] = c[d] + (c[d] - pts[worst][d]);
        const double fr = f(r);
        if (fr < val[best]) {
            for (int d = 0; d < 3; ++d) e[d] = c[d] + 2.0 * (c[d] - pts[worst][d]);
            const double fe = f(e);
            if (fe < fr) {
                for (int d = 0; d < 3; ++d) pts[worst][d] = e[d];
                val[worst] = fe;
            } else {
                for (int d = 0; d < 3; ++d) pts[worst][d] = r[d];
                val[worst] = fr;
            }
        } else if (fr < val[second]) {
            for (int d = 0; d < 3; ++d) pts[worst][d] = r[d];
            val[worst] = fr;
        } else {
            for (int d = 0; d < 3; ++d) q[d] = c[d] + 0.5 * (pts[worst][d] - c[d]);
            const double fq = f(q);
            if (fq < val[worst]) {
                for (int d = 0; d < 3; ++d) pts[worst][d] = q[d];
                val[worst] = fq;
            } else {
                for (int k = 0; k < 4; ++k)
                    if (k != best) {
                        for (int d = 0; d < 3; ++d) pts[k][d] = pts[best][d] + 0.5 * (pts[k][d] - pts[best][d]);
                        val[k] = f(pts[k]);
                    }
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k)
        if (val[k] < val[best]) best = k;
    l.m11 = std::max(pts[best][0], 1e-5);
    l.m22 = std::max(pts[best][1], 1e-5);
    l.m13 = pts[best][2];
    l.update();
}

std::vector<float> fitLtc() {
    std::vector<float> table(kLtcN * kLtcN * 4);
    for (unsigned j = 0; j < kLtcN; ++j) {
        const double u = static_cast<double>(j) / (kLtcN - 1);
        const double mu = std::max(1.0 - u * u, 1e-3);
        const V3 V{std::sqrt(1.0 - mu * mu), 0.0, mu};
        Ltc l;
        for (int i = static_cast<int>(kLtcN) - 1; i >= 0; --i) {
            const double rough = std::max(static_cast<double>(i) / (kLtcN - 1), 0.05);
            const double alpha = rough * rough;
            // The lobe's normalization and mean direction (its symmetry plane is y = 0).
            double norm = 0.0;
            V3 avg{0, 0, 0};
            constexpr unsigned n = 64;
            for (unsigned a = 0; a < n; ++a)
                for (unsigned b = 0; b < n; ++b) {
                    const V3 L = ggxSample(V, alpha, (a + 0.5) / n, (b + 0.5) / n);
                    if (L.z <= 0.0) continue;
                    const double w = ggxSingle(V, L, alpha * alpha) / ggxPdf(V, L, alpha * alpha);
                    norm += w;
                    avg = avg + L * w;
                }
            norm /= n * n;
            avg.y = 0.0;
            avg = normalized(avg);
            l.Z = avg;
            l.Y = {0, 1, 0};
            l.X = cross(l.Y, l.Z);
            if (i == static_cast<int>(kLtcN) - 1) {   // the roughest: an isotropic start
                l.m11 = l.m22 = std::max(alpha, 1e-3);
                l.m13 = 0.0;
            }
            l.update();
            ltcFit(l, V, alpha, norm);
            const double s = l.inv.m[1][1];
            float* out = &table[(static_cast<unsigned>(i) * kLtcN + j) * 4];
            out[0] = static_cast<float>(l.inv.m[0][0] / s);
            out[1] = static_cast<float>(l.inv.m[0][2] / s);
            out[2] = static_cast<float>(l.inv.m[2][0] / s);
            out[3] = static_cast<float>(l.inv.m[2][2] / s);
        }
    }
    return table;
}

std::vector<float> build() {
    std::vector<float> e1(kMu * kRough), s5(kMu * kRough), s6(kMu * kRough), e1Avg(kRough), favg(kEta);
    std::vector<float> essD(kDMu * kDRough * kDEta), essDLow(kDMu * kDRough * kDEta);
    for (unsigned r = 0; r < kRough; ++r)
        for (unsigned m = 0; m < kMu; ++m) {
            const auto w = integrate(muAt(m, kMu), gridAt(r, kRough), 1.5f, kSamples);
            e1[r * kMu + m] = static_cast<float>(w[0]);
            s5[r * kMu + m] = static_cast<float>(w[1]);
            s6[r * kMu + m] = static_cast<float>(w[2]);
        }
    // E1's average over the interpolated table, as the lookups read it.
    for (unsigned r = 0; r < kRough; ++r) {
        double sum = 0.0;
        constexpr unsigned kSteps = 4096;
        for (unsigned k = 0; k < kSteps; ++k) {
            const double mu = (k + 0.5) / kSteps;
            const double g = std::sqrt(mu) * (kMu - 1);
            const unsigned i = std::min(static_cast<unsigned>(g), kMu - 2);
            const double f = g - i;
            const double v = e1[r * kMu + i] * (1.0 - f) + e1[r * kMu + i + 1] * f;
            sum += v * mu;
        }
        e1Avg[r] = static_cast<float>(2.0 * sum / kSteps);
    }
    // The dielectric Fresnel's average, 2 * integral F(mu) mu dmu.
    for (unsigned e = 0; e < kEta; ++e) {
        const float eta = 1.0f + (kEtaMax - 1.0f) * gridAt(e, kEta);
        constexpr unsigned kSteps = 8192;
        std::vector<Item> items(kSteps);
        for (unsigned k = 0; k < kSteps; ++k) {
            items[k].op = Op::Fresnel;
            items[k].wo = {(k + 0.5f) / kSteps, eta, 0.0f};
        }
        ohao::material::run(items, {});
        double sum = 0.0;
        for (unsigned k = 0; k < kSteps; ++k) sum += items[k].r0[0] * ((k + 0.5) / kSteps);
        favg[e] = static_cast<float>(2.0 * sum / kSteps);
    }
    for (unsigned e = 0; e < kDEta; ++e)
        for (unsigned r = 0; r < kDRough; ++r)
            for (unsigned m = 0; m < kDMu; ++m) {
                const float eta = 1.0f + (kEtaMax - 1.0f) * gridAt(e, kDEta);
                const auto w = integrate(muAt(m, kDMu), gridAt(r, kDRough), eta, kDielSamples);
                essD[(e * kDRough + r) * kDMu + m] = static_cast<float>(w[3]);
            }
    for (unsigned e = 0; e < kDEta; ++e)
        for (unsigned r = 0; r < kDRough; ++r)
            for (unsigned m = 0; m < kDMu; ++m) {
                const float g = gridAt(e, kDEta);
                const float eta = 1.0f + kLowEtaSpan * g * g;
                const auto w = integrate(muAt(m, kDMu), gridAt(r, kDRough), eta, kDielSamples);
                essDLow[(e * kDRough + r) * kDMu + m] = static_cast<float>(w[3]);
            }
    // Anisotropy: the isotropic roughness whose E1 (as the lookups read E1) is the anisotropic
    // lobe's single-scatter albedo at (mu, phi), and that albedo's average.
    auto e1Lookup = [&](double mu, double rough) {
        const double gm = std::sqrt(std::clamp(mu, 0.0, 1.0)) * (kMu - 1);
        const unsigned im = std::min(static_cast<unsigned>(gm), kMu - 2);
        const double fm = gm - im;
        const double gr = std::clamp(rough, 0.0, 1.0) * (kRough - 1);
        const unsigned ir = std::min(static_cast<unsigned>(gr), kRough - 2);
        const double fr = gr - ir;
        auto at = [&](unsigned r, unsigned m) { return static_cast<double>(e1[r * kMu + m]); };
        const double a = at(ir, im) * (1.0 - fm) + at(ir, im + 1) * fm;
        const double b = at(ir + 1, im) * (1.0 - fm) + at(ir + 1, im + 1) * fm;
        return a * (1.0 - fr) + b * fr;
    };
    std::vector<float> anisoRough(kAA * kAR * kAM * kAP), anisoE1Avg(kAA * kAR), anisoS56(2 * kAA * kAR * kAM * kAP);
    for (unsigned a = 0; a < kAA; ++a)
        for (unsigned r = 0; r < kAR; ++r)
            for (unsigned m = 0; m < kAM; ++m)
                for (unsigned p = 0; p < kAP; ++p) {
                    const unsigned cell = ((a * kAR + r) * kAM + m) * kAP + p;
                    float& out = anisoRough[cell];
                    const float rough = gridAt(r, kAR);
                    const float mu = std::max(muAt(m, kAM), kMinMu);
                    const float phi = 0.5f * 3.14159265f * gridAt(p, kAP);
                    const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
                    std::vector<Item> items(kAnisoSamples);
                    for (unsigned k = 0; k < kAnisoSamples; ++k) {
                        Item& it = items[k];
                        it.op = Op::Aniso;
                        it.material.specularRoughness = std::max(rough, kMinRough);
                        it.material.specularRoughnessAnisotropy = anisoAt(a);
                        it.wo = {s * std::cos(phi), s * std::sin(phi), mu};
                        it.v = {(static_cast<float>(k) + 0.5f) / static_cast<float>(kAnisoSamples), radicalInverse(k), 0.0f};
                    }
                    ohao::material::run(items, {});
                    double e = 0.0, e5 = 0.0, e6 = 0.0;
                    for (const Item& it : items) {
                        e += it.r0[0];
                        e5 += it.r0[1];
                        e6 += it.r0[2];
                    }
                    e /= kAnisoSamples;
                    anisoS56[2 * cell] = static_cast<float>(e5 / kAnisoSamples);
                    anisoS56[2 * cell + 1] = static_cast<float>(e6 / kAnisoSamples);
                    if (a == 0) {   // isotropic: the roughness itself
                        out = rough;
                        continue;
                    }
                    // E1 falls with roughness: bisect for the roughness that reads e.
                    double lo = 0.0, hi = 1.0;
                    if (e >= e1Lookup(mu, 0.0)) hi = 0.0;
                    else if (e <= e1Lookup(mu, 1.0)) lo = 1.0;
                    else
                        for (int k = 0; k < 40; ++k) {
                            const double mid = 0.5 * (lo + hi);
                            (e1Lookup(mu, mid) > e ? lo : hi) = mid;
                        }
                    out = static_cast<float>(0.5 * (lo + hi));
                }
    // The average of E1 at those roughnesses, as the lookups read them: 2 x the integral of
    // E(mu, phi) mu dmu over phi's quarter (the lobe's albedo is symmetric about both axes).
    for (unsigned a = 0; a < kAA; ++a)
        for (unsigned r = 0; r < kAR; ++r) {
            constexpr unsigned kStepsMu = 512, kStepsPhi = 32;
            double sum = 0.0;
            for (unsigned k = 0; k < kStepsMu; ++k)
                for (unsigned j = 0; j < kStepsPhi; ++j) {
                    const double mu = (k + 0.5) / kStepsMu;
                    const double gm = std::sqrt(mu) * (kAM - 1);
                    const unsigned im = std::min(static_cast<unsigned>(gm), kAM - 2);
                    const double fm = gm - im;
                    const double gp = (j + 0.5) / kStepsPhi * (kAP - 1);
                    const unsigned ip = std::min(static_cast<unsigned>(gp), kAP - 2);
                    const double fp = gp - ip;
                    auto at = [&](unsigned m, unsigned p) {
                        return static_cast<double>(anisoRough[((a * kAR + r) * kAM + m) * kAP + p]);
                    };
                    const double rr = (at(im, ip) * (1.0 - fp) + at(im, ip + 1) * fp) * (1.0 - fm)
                                    + (at(im + 1, ip) * (1.0 - fp) + at(im + 1, ip + 1) * fp) * fm;
                    sum += e1Lookup(mu, rr) * mu;
                }
            anisoE1Avg[a * kAR + r] = static_cast<float>(2.0 * sum / (kStepsMu * kStepsPhi));
        }
    std::vector<float> all;
    // Step 3: the glass's interface from both sides (op 8: refraction and reflection through the
    // same visible normals; op 4's dielectric weight for the reflection inside, as EssD's).
    auto glass = [&](float mu, float rough, float eta) {
        mu = std::max(mu, kMinMu);
        rough = std::max(rough, kMinRough);
        std::vector<Item> items(kDielSamples);
        const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
        for (unsigned k = 0; k < kDielSamples; ++k) {
            Item& it = items[k];
            it.op = Op::Glass;
            it.material.specularRoughness = rough;
            it.wo = {s, 0.0f, mu};
            it.v = {(static_cast<float>(k) + 0.5f) / static_cast<float>(kDielSamples), radicalInverse(k), eta};
        }
        ohao::material::run(items, {});
        double t = 0.0;
        for (const Item& it : items) t += it.r0[0];
        return t / kDielSamples;
    };
    std::vector<float> essT(kDMu * kDRough * kGEta), essDIn(kDMu * kDRough * kGEta), essTIn(kDMu * kDRough * kGEta);
    for (unsigned e = 0; e < kGEta; ++e)
        for (unsigned r = 0; r < kDRough; ++r)
            for (unsigned m = 0; m < kDMu; ++m) {
                const float n = 1.0f + (kEtaMax - 1.0f) * gridAt(e, kGEta);
                const unsigned i = (e * kDRough + r) * kDMu + m;
                essT[i] = static_cast<float>(glass(muAt(m, kDMu), gridAt(r, kDRough), n));
                essTIn[i] = static_cast<float>(glass(muAt(m, kDMu), gridAt(r, kDRough), 1.0f / n));
                essDIn[i] = static_cast<float>(integrate(muAt(m, kDMu), gridAt(r, kDRough), 1.0f / n, kDielSamples)[3]);
            }
    std::vector<float> ltc = fitLtc();
    for (const auto* t : {&e1, &s5, &s6, &e1Avg, &favg, &essD, &essDLow, &anisoRough, &anisoE1Avg, &anisoS56, &essT,
                          &essDIn, &essTIn, &ltc})
        all.insert(all.end(), t->begin(), t->end());
    return all;
}

std::string header(const std::vector<float>& t) {
    std::ostringstream o;
    o << "#pragma once\n\n"
         "// OpenPBR's energy tables (shaders/material/openpbr.slang, OpenPBRTables: its layout and lookups).\n"
         "// Generated by tools/material/openpbr_tables from the module's own code; do not edit. Regenerate:\n"
         "//   build/Release/openpbr_tables ohao/render/material/openpbr_tables.hpp\n\n"
         "#include <cstddef>\n\n"
         "namespace ohao::render {\n\n"
         "inline constexpr std::size_t kOpenPBRTableFloats = "
      << t.size() << ";\ninline constexpr float kOpenPBRTables[kOpenPBRTableFloats] = {\n";
    char buf[32];
    for (size_t i = 0; i < t.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%.9g", t[i]);
        if (!std::strpbrk(buf, ".e")) std::strncat(buf, ".0", sizeof buf - std::strlen(buf) - 1);   // 1f is no literal
        o << (i % 8 == 0 ? "    " : " ") << buf << "f," << (i % 8 == 7 || i + 1 == t.size() ? "\n" : "");
    }
    o << "};\n\n} // namespace ohao::render\n";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    const bool check = argc == 3 && std::string(argv[1]) == "--check";
    if (argc != 2 && !check) {
        std::fprintf(stderr, "usage: openpbr_tables [--check] OUT.hpp\n");
        return 2;
    }
    const std::string path = argv[argc - 1];
    const std::string text = header(build());
    if (check) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream have;
        have << in.rdbuf();
        const bool same = have.str() == text;
        std::printf("{\"suite\":\"openpbr_tables\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", same ? 1 : 0,
                    same ? 0 : 1, same ? "" : "\"the tables are not what this build generates: regenerate them\"");
        return same ? 0 : 1;
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    std::printf("openpbr_tables: wrote %s (%zu bytes)\n", path.c_str(), text.size());
    return out ? 0 : 1;
}
