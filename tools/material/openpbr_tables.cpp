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
    std::vector<float> anisoRough(kAA * kAR * kAM * kAP), anisoE1Avg(kAA * kAR);
    for (unsigned a = 0; a < kAA; ++a)
        for (unsigned r = 0; r < kAR; ++r)
            for (unsigned m = 0; m < kAM; ++m)
                for (unsigned p = 0; p < kAP; ++p) {
                    float& out = anisoRough[((a * kAR + r) * kAM + m) * kAP + p];
                    const float rough = gridAt(r, kAR);
                    if (a == 0) {   // isotropic: the roughness itself
                        out = rough;
                        continue;
                    }
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
                    double e = 0.0;
                    for (const Item& it : items) e += it.r0[0];
                    e /= kAnisoSamples;
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
    for (const auto* t : {&e1, &s5, &s6, &e1Avg, &favg, &essD, &essDLow, &anisoRough, &anisoE1Avg})
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
