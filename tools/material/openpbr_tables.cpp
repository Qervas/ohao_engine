// OpenPBR's energy tables (shaders/material/openpbr.slang, OpenPBRTables), integrated from the
// module's own code on the CPU and written as ohao/render/material/openpbr_tables.hpp:
//
//   E1, S5, S6  [rough][mu], 32 x 32: GGX single scatter's directional albedo with F = 1, and
//               weighted by (1 - c)^5 and c (1 - c)^6 (c = m.wo): F82's albedo is linear in them
//   E1Avg       [rough], 32: 2 * integral of the interpolated E1(mu) mu dmu
//   FavgD       [eta], 32, eta in [1, 3]: the exact dielectric Fresnel's cosine-weighted average
//   EssD        [eta][rough][mu], 16^3: the dielectric's single-scatter albedo, exact Fresnel
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
    std::vector<float> essD(kDMu * kDRough * kDEta);
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
    std::vector<float> all;
    for (const auto* t : {&e1, &s5, &s6, &e1Avg, &favg, &essD}) all.insert(all.end(), t->begin(), t->end());
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
