// OpenPBR's base on the CPU (docs/plans/2026-10-29-openpbr-step1.md, T1): shaders/material/openpbr.slang
// run through its C++ build (tools/material/openpbr_cpu). Every test reads the module's own code:
//
//   t1_defaults            the parameters' defaults are OpenPBR 1.1.1's
//   t1_fresnel             the exact dielectric Fresnel: 0.04 head-on at 1.5, 0 at 1, 1 at grazing
//   t1_weight_zero         specular_weight 0 over a smooth diffuse (EON at roughness 0, Lambert) of
//                          albedo 0.5 reflects exactly 0.5
//   t1_mirror_metal        a near-mirror metal seen head-on reflects its F0 (F82 at mu = 1)
//   t1_reciprocity         each lobe is symmetric in (wo, wi): GGX single scatter, EON, the
//                          multiple-scattering lobe (the layered whole is not: OpenPBR scales the
//                          diffuse by the specular albedo at wo alone)
//   t1_sampling_chi2       sample() draws directions with the density pdf() says (chi-square over
//                          a 32 x 64 hemisphere grid, p above a Sidak-corrected 1%) and weights
//                          them eval / pdf
//   t1_albedo              albedo(wo) against the mean sample weight (2^18 samples), within 0.5%
//   t1_white_furnace       a white dielectric (IOR 1.5 and 3) over white EON and a white metal
//                          reflect what they receive, within 1%, at any roughness and view up to
//                          85 degrees
#include "openpbr_cpu.hpp"
#include "render/material/openpbr_tables.hpp"
#include "common/harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace {

using ohao::material::Float3;
using ohao::material::Item;
using ohao::material::Material;
using ohao::material::Op;

constexpr double kPi = 3.14159265358979323846;
const std::span<const float> kTables(ohao::render::kOpenPBRTables, ohao::render::kOpenPBRTableFloats);

uint32_t pcg(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(uint32_t& s) {
    s = pcg(s);
    return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
}

Float3 view(float mu) { return {std::sqrt(std::max(0.0f, 1.0f - mu * mu)), 0.0f, mu}; }

float lum(const std::array<float, 4>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

Item one(Op op, const Material& m, Float3 wo, Float3 v) {
    Item it;
    it.op = op;
    it.material = m;
    it.wo = wo;
    it.v = v;
    ohao::material::run(std::span<Item>(&it, 1), kTables);
    return it;
}

// The mean sample weight over n samples: an unbiased estimate of the albedo.
std::array<double, 3> meanWeight(const Material& m, Float3 wo, unsigned n, uint32_t seed) {
    std::vector<Item> items(n);
    for (unsigned k = 0; k < n; ++k) {
        items[k].op = Op::Sample;
        items[k].material = m;
        items[k].wo = wo;
        items[k].v = {rnd(seed), rnd(seed), rnd(seed)};
    }
    ohao::material::run(items, kTables);
    std::array<double, 3> sum{};
    for (const Item& it : items)
        for (int c = 0; c < 3; ++c) sum[c] += it.r1[c];
    for (double& v : sum) v /= n;
    return sum;
}

std::string describe(const Material& m, float mu) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "metal %.2g rough %.2g diffRough %.2g weight %.2g ior %.2g color %.2g mu %.2g",
                  m.baseMetalness, m.specularRoughness, m.baseDiffuseRoughness, m.specularWeight, m.specularIor,
                  m.baseColor[0], mu);
    return buf;
}

void t1_defaults() {
    const Material m;
    CHECK(m.baseColor[0] == 0.8f && m.baseColor[1] == 0.8f && m.baseColor[2] == 0.8f, "base_color 0.8");
    CHECK(m.baseWeight == 1.0f && m.baseMetalness == 0.0f && m.baseDiffuseRoughness == 0.0f, "base weights");
    CHECK(m.specularWeight == 1.0f && m.specularColor[0] == 1.0f && m.specularRoughness == 0.3f, "specular");
    CHECK(m.specularIor == 1.5f, "specular_ior 1.5");
}

void t1_fresnel() {
    const Item head = one(Op::Fresnel, Material{}, {1.0f, 1.5f, 0.0f}, {});
    CHECK(std::abs(head.r0[0] - 0.04f) < 1e-6f, "F(1, 1.5) = 0.04");
    const Item same = one(Op::Fresnel, Material{}, {0.7f, 1.0f, 0.0f}, {});
    CHECK(std::abs(same.r0[0]) < 1e-6f, "F at eta 1 is 0");
    const Item graze = one(Op::Fresnel, Material{}, {1e-4f, 1.5f, 0.0f}, {});
    CHECK(graze.r0[0] > 0.99f, "F at grazing is 1");
}

void t1_weight_zero() {
    Material m;
    m.specularWeight = 0.0f;
    m.baseColor = {0.5f, 0.5f, 0.5f};
    m.baseDiffuseRoughness = 0.0f;
    for (float mu : {1.0f, 0.5f, 0.1f}) {
        const Item a = one(Op::Albedo, m, view(mu), {});
        CHECK(std::abs(a.r0[0] - 0.5f) < 1e-4f, ("albedo 0.5 at mu " + std::to_string(mu)).c_str());
    }
}

void t1_mirror_metal() {
    Material m;
    m.baseMetalness = 1.0f;
    m.baseColor = {0.9f, 0.6f, 0.3f};
    m.specularRoughness = 0.01f;
    const Item a = one(Op::Albedo, m, view(1.0f), {});
    for (int c = 0; c < 3; ++c) CHECK(std::abs(a.r0[c] - m.baseColor[c]) < 0.01f, "albedo is F0 head-on");
}

void t1_reciprocity() {
    uint32_t seed = 7u;
    for (int k = 0; k < 2000; ++k) {
        Material m;
        m.baseColor = {0.3f + 0.7f * rnd(seed), 0.5f, 0.9f};
        m.specularRoughness = 0.05f + 0.95f * rnd(seed);
        m.baseDiffuseRoughness = rnd(seed);
        auto dir = [&] {
            const float mu = 0.02f + 0.98f * rnd(seed), phi = 6.2831853f * rnd(seed);
            const float s = std::sqrt(1.0f - mu * mu);
            return Float3{s * std::cos(phi), s * std::sin(phi), mu};
        };
        const Float3 a = dir(), b = dir();
        const Item ab = one(Op::Lobes, m, a, b), ba = one(Op::Lobes, m, b, a);
        for (int c = 0; c < 4; ++c) {
            const float x = ab.r0[c], y = ba.r0[c];
            CHECK(std::abs(x - y) <= 1e-4f * std::max(1.0f, std::abs(x)), "a lobe is not reciprocal");
        }
        CHECK(std::abs(ab.r1[0] - ba.r1[0]) <= 1e-4f * std::max(1.0f, std::abs(ab.r1[0])), "the ms lobe is not reciprocal");
    }
}

// The chi-square distribution's upper tail by Wilson-Hilferty (accurate for the 100s of degrees
// of freedom here).
double chi2Tail(double x, double dof) {
    const double z = (std::cbrt(x / dof) - (1.0 - 2.0 / (9.0 * dof))) / std::sqrt(2.0 / (9.0 * dof));
    return 0.5 * std::erfc(z / std::sqrt(2.0));
}

struct Case {
    Material m;
    float mu;
};

std::vector<Case> lobeCases() {
    std::vector<Case> cases;
    for (float rough : {0.3f, 0.6f, 1.0f})
        for (float mu : {0.95f, 0.6f, 0.2f}) {
            Material d;
            d.specularRoughness = rough;
            d.baseDiffuseRoughness = 0.5f;
            cases.push_back({d, mu});
            Material dense = d;
            dense.specularIor = 3.0f;
            dense.baseColor = {0.2f, 0.5f, 0.9f};
            cases.push_back({dense, mu});
            Material metal = d;
            metal.baseMetalness = 1.0f;
            metal.baseColor = {0.95f, 0.64f, 0.54f};
            cases.push_back({metal, mu});
        }
    for (float diffRough : {0.0f, 0.5f, 1.0f})
        for (float mu : {0.95f, 0.6f, 0.2f}) {
            Material e;
            e.specularWeight = 0.0f;
            e.baseDiffuseRoughness = diffRough;
            cases.push_back({e, mu});
        }
    return cases;
}

void t1_sampling_chi2() {
    constexpr unsigned kTheta = 32, kPhi = 64, kSub = 6, kN = 1u << 20;
    const std::vector<Case> cases = lobeCases();
    const double threshold = 1.0 - std::pow(1.0 - 0.01, 1.0 / cases.size());   // Sidak
    uint32_t seed = 11u;
    for (const Case& c : cases) {
        // Expected counts: the pdf integrated over each bin (cos theta uniform bins, phi bins).
        std::vector<Item> grid;
        grid.reserve(kTheta * kPhi * kSub * kSub);
        for (unsigned t = 0; t < kTheta; ++t)
            for (unsigned p = 0; p < kPhi; ++p)
                for (unsigned i = 0; i < kSub; ++i)
                    for (unsigned j = 0; j < kSub; ++j) {
                        const float mu = (t + (i + 0.5f) / kSub) / kTheta;
                        const float phi = static_cast<float>(2.0 * kPi) * (p + (j + 0.5f) / kSub) / kPhi - static_cast<float>(kPi);
                        const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
                        Item it;
                        it.op = Op::Pdf;
                        it.material = c.m;
                        it.wo = view(c.mu);
                        it.v = {s * std::cos(phi), s * std::sin(phi), mu};
                        grid.push_back(it);
                    }
        ohao::material::run(grid, kTables);
        std::vector<double> expected(kTheta * kPhi, 0.0);
        const double cell = (1.0 / kTheta) * (2.0 * kPi / kPhi) / (kSub * kSub);
        for (size_t k = 0; k < grid.size(); ++k) expected[k / (kSub * kSub)] += grid[k].r0[0] * cell * kN;
        // Observed counts, and each sample's weight against eval / pdf.
        std::vector<Item> samples(kN);
        for (Item& it : samples) {
            it.op = Op::Sample;
            it.material = c.m;
            it.wo = view(c.mu);
            it.v = {rnd(seed), rnd(seed), rnd(seed)};
        }
        ohao::material::run(samples, kTables);
        std::vector<double> observed(kTheta * kPhi, 0.0);
        std::vector<Item> checks;
        for (const Item& it : samples) {
            if (!(it.r0[3] > 0.0f)) continue;
            const float mu = std::clamp(it.r0[2], 0.0f, 0.999999f);
            float phi = std::atan2(it.r0[1], it.r0[0]);
            const unsigned t = std::min(static_cast<unsigned>(mu * kTheta), kTheta - 1);
            const unsigned p = std::min(static_cast<unsigned>((phi + kPi) / (2.0 * kPi) * kPhi), kPhi - 1);
            observed[t * kPhi + p] += 1.0;
            if (checks.size() < 4096) {
                Item e;
                e.op = Op::Eval;
                e.material = c.m;
                e.wo = view(c.mu);
                e.v = {it.r0[0], it.r0[1], it.r0[2]};
                e.r1 = {it.r0[3], it.r1[0], it.r1[1], it.r1[2]};   // pdf, weight
                checks.push_back(e);
            }
        }
        std::vector<Item> evals = checks;
        ohao::material::run(evals, kTables);
        for (size_t k = 0; k < evals.size(); ++k) {
            const float pdf = checks[k].r1[0];
            for (int ch = 0; ch < 3; ++ch) {
                const float want = evals[k].r0[ch] / pdf, got = checks[k].r1[1 + ch];
                CHECK(std::abs(want - got) <= 1e-3f * std::max(1.0f, std::abs(want)),
                      ("weight != eval / pdf: " + describe(c.m, c.mu)).c_str());
            }
        }
        // Pool bins expected under 5 into one.
        double chi2 = 0.0, poolE = 0.0, poolO = 0.0;
        int dof = -1;
        for (size_t k = 0; k < expected.size(); ++k) {
            if (expected[k] < 5.0) {
                poolE += expected[k];
                poolO += observed[k];
                continue;
            }
            chi2 += (observed[k] - expected[k]) * (observed[k] - expected[k]) / expected[k];
            ++dof;
        }
        if (poolE >= 5.0) {
            chi2 += (poolO - poolE) * (poolO - poolE) / poolE;
            ++dof;
        }
        const double p = chi2Tail(chi2, std::max(dof, 1));
        if (!(p > threshold))
            std::printf("\n    chi2 %.1f dof %d p %.3g: %s", chi2, dof, p, describe(c.m, c.mu).c_str());
        CHECK(p > threshold, ("sample() is not drawn with pdf(): " + describe(c.m, c.mu)).c_str());
    }
}

void t1_albedo() {
    uint32_t seed = 23u;
    for (const Case& c : lobeCases()) {
        const Item a = one(Op::Albedo, c.m, view(c.mu), {});
        const auto mc = meanWeight(c.m, view(c.mu), 1u << 18, seed++);
        for (int ch = 0; ch < 3; ++ch) {
            const double rel = std::abs(a.r0[ch] - mc[ch]) / std::max(mc[ch], 1e-3);
            if (!(rel <= 0.005))
                std::printf("\n    albedo %.5f vs integral %.5f: %s", a.r0[ch], mc[ch], describe(c.m, c.mu).c_str());
            CHECK(rel <= 0.005, ("albedo() is not the integral of eval(): " + describe(c.m, c.mu)).c_str());
        }
    }
}

void t1_white_furnace() {
    uint32_t seed = 31u;
    for (float rough : {0.01f, 0.3f, 0.6f, 1.0f})
        for (float diffRough : {0.0f, 1.0f})
            for (float deg : {0.0f, 30.0f, 60.0f, 85.0f}) {
                const float mu = std::cos(deg * static_cast<float>(kPi) / 180.0f);
                Material white;
                white.baseColor = {1.0f, 1.0f, 1.0f};
                white.specularRoughness = rough;
                white.baseDiffuseRoughness = diffRough;
                Material dense = white;   // IOR 3: the dielectric's multiple scattering is ~2% here
                dense.specularIor = 3.0f;
                Material metal = white;
                metal.baseMetalness = 1.0f;
                for (const Material& m : {white, dense, metal}) {
                    const Item a = one(Op::Albedo, m, view(mu), {});
                    const auto mc = meanWeight(m, view(mu), 1u << 18, seed++);
                    if (!(std::abs(a.r0[0] - 1.0f) <= 0.01f && std::abs(mc[0] - 1.0) <= 0.01))
                        std::printf("\n    albedo %.5f integral %.5f: %s", a.r0[0], mc[0], describe(m, mu).c_str());
                    CHECK(std::abs(a.r0[0] - 1.0f) <= 0.01f, ("albedo() is not 1: " + describe(m, mu)).c_str());
                    CHECK(std::abs(mc[0] - 1.0) <= 0.01, ("the integral of eval() is not 1: " + describe(m, mu)).c_str());
                }
            }
}

} // namespace

int main() {
    std::printf("material_tests (OpenPBR's base on the CPU)\n");
    RUN(t1_defaults);
    RUN(t1_fresnel);
    RUN(t1_weight_zero);
    RUN(t1_mirror_metal);
    RUN(t1_reciprocity);
    RUN(t1_sampling_chi2);
    RUN(t1_albedo);
    RUN(t1_white_furnace);
    return ohao::test::finish("material_tests");
}
