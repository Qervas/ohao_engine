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
//
// Step 2 (docs/plans/2026-10-31-openpbr-step2.md, T1): the coat, the fuzz and anisotropy.
//
//   t2_defaults            the coat's, the fuzz's and anisotropy's defaults are OpenPBR 1.1.1's
//   t2_reciprocity         the anisotropic GGX lobe and the coat's lobe are symmetric in (wo, wi)
//   t2_sampling_chi2       t1_sampling_chi2 over coated, fuzzy, anisotropic and layered materials
//   t2_albedo              t1_albedo over the same, anisotropic lobes (AnisoS56) at 18 to 81 degrees
//                          along and across the tangent: metals within 1%, a dielectric 2.5%
//   t2_white_furnace       a white coat (darkening off and on) over a white dielectric and a white
//                          metal, and white fuzz over a white dielectric, within 1% at any
//                          roughness and view up to 85 degrees
//   t2_aniso_furnace       a white anisotropic metal (0.5, 0.9) seen along and across the tangent
//                          (2^20 samples): within 1% to 60 degrees, 5% at 85, at roughness 0.01 to
//                          0.6 (past that alpha_t leaves GGX's tables at alpha 1, a known gap)
//
// Step 3 (docs/plans/2026-10-31-openpbr-step3.md, T1): transmission, thin-walled, thin film, dispersion.
//
//   t3_defaults            transmission's, the thin film's and thin-walled's defaults are OpenPBR 1.1.1's
//   t3_sampling_chi2       sample() drawn with pdf() over the whole sphere for glass (solid from
//                          outside and inside, thin-walled, half transmissive, coated), and its weights
//                          eval / pdf
//   t3_albedo              albedo() the integral of eval() over the sphere for transmissive materials
//   t3_white_furnace       white glass (IOR 1.33, 1.5 and 2.4, roughness 0.01 to 1, solid from outside and
//                          inside, thin-walled from both sides, half diffuse) reflects and refracts exactly
//                          what it receives: within 1%
//   t3_thin_film           the Airy reflectance in [0, 1], at no thickness the base's Fresnel, coloured
//                          (its channels apart) at 300 nm; a filmed white metal still within 1% of white
//   t3_dispersion          the IOR at the d line (587.56 nm) is specular_ior; blue above red
//
// Step 4 (docs/plans/2026-11-01-openpbr-step4.md, T1): subsurface.
//
//   t4_defaults            subsurface's defaults are OpenPBR 1.1.1's
//   t4_van_de_hulst        the single-scatter albedo of a black volume is 0, of a white one 1, rising in between
//   t4_local               the local subsurface (a diffuse lobe of subsurface_color): albedo() the integral of eval(),
//                          sampled with pdf(), a white one within 1% of white
//   t4_walked              walked (lobe 5, the path tracer's): what enters is 1 - the specular's albedo, white
//                          within 1% when the walk returns all of it, and pdf() leaves the walk out
//
// Area lights by linearly transformed cosines (OpenPBR step 2, T5):
//
//   t5_ltc_normalized      the fitted LTC's density integrates to 1 over the sphere
//   t5_ltc_fit             it stays near the normalized GGX lobe it was fitted to: their L1 distance
//   t5_ltc_quad            ltcQuad (the horizon clip, the edge integral) is the density's integral over a
//                          quad, against Monte Carlo: small and large, crossing the horizon
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

// A view at cosine mu, phi from the tangent (an anisotropic lobe's x).
Float3 view(float mu, float phi = 0.0f) {
    const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
    return {s * std::cos(phi), s * std::sin(phi), mu};
}

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
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "metal %.2g rough %.2g aniso %.2g diffRough %.2g weight %.2g ior %.2g color %.2g coat %.2g/%.2g/%.2g "
                  "ior %.2g color %.2g dark %.2g fuzz %.2g/%.2g color %.2g mu %.2g",
                  m.baseMetalness, m.specularRoughness, m.specularRoughnessAnisotropy, m.baseDiffuseRoughness,
                  m.specularWeight, m.specularIor, m.baseColor[0], m.coatWeight, m.coatRoughness,
                  m.coatRoughnessAnisotropy, m.coatIor, m.coatColor[0], m.coatDarkening, m.fuzzWeight, m.fuzzRoughness,
                  m.fuzzColor[0], mu);
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
    double albedoTol{0.005};   // albedo() against the integral of eval()
    float phi{0.0f};           // the view's azimuth from the tangent
    bool chi2{true};           // in the sampling test (false: sampled as another case is)
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

// Coated, fuzzy, anisotropic and layered materials (step 2).
std::vector<Case> layeredCases() {
    std::vector<Case> cases;
    for (float mu : {0.95f, 0.5f, 0.15f}) {
        Material car;   // a car paint: a smooth coat over a rough metal
        car.baseMetalness = 1.0f;
        car.baseColor = {0.1f, 0.25f, 0.8f};
        car.specularRoughness = 0.4f;
        car.coatWeight = 1.0f;
        car.coatRoughness = 0.25f;   // the grid's 6 x 6 points a bin cannot integrate a sharper peak
        // The coat's EssD (16 points of mu) reads 0.54% low at 81 degrees and 0.57% high at 60 (4M
        // samples): 1% for coats off normal.
        cases.push_back({car, mu, mu < 0.9f ? 0.01 : 0.005});
        Material lacquer;   // a tinted, half-rough, partial coat over a dielectric
        lacquer.baseColor = {0.6f, 0.35f, 0.15f};
        lacquer.coatWeight = 0.7f;
        lacquer.coatRoughness = 0.3f;
        lacquer.coatColor = {0.9f, 0.7f, 0.4f};
        lacquer.coatIor = 1.5f;
        // A coloured coat's absorption is exp-weighted along wi; albedo() takes it along the
        // diffuse's mean path (Jensen's gap): 1%.
        cases.push_back({lacquer, mu, 0.01});
        // A dark base's albedo is mostly its 4% specular, whose table (EssD, step 1) reads 0.8% off at
        // mu 0.95 there: the base alone shows it, so the fuzz over it is held to 1% too.
        Material dark;
        dark.baseColor = {0.3f, 0.02f, 0.05f};
        cases.push_back({dark, mu, 0.01});
        Material velvet;
        velvet.baseColor = {0.3f, 0.02f, 0.05f};
        velvet.fuzzWeight = 1.0f;
        velvet.fuzzColor = {0.9f, 0.2f, 0.3f};
        velvet.fuzzRoughness = 0.5f;
        cases.push_back({velvet, mu, 0.01});
        Material silk = velvet;
        silk.fuzzRoughness = 0.15f;
        cases.push_back({silk, mu, 0.01});
    }
    // After the cases above, so their seeds stay.
    for (float mu : {0.95f, 0.5f, 0.15f}) {
        Material car;
        car.baseMetalness = 1.0f;
        car.baseColor = {0.1f, 0.25f, 0.8f};
        car.specularRoughness = 0.4f;
        car.coatWeight = 1.0f;
        car.coatRoughness = 0.25f;
        // Anisotropic lobes (AnisoRough for E1, AnisoS56 for the Fresnel-weighted moments), seen along
        // the tangent and across it. Measured at 4M samples, the worst view: the metals 0.42% (F82's
        // albedo is linear in E1, S5 and S6: exact but for the tables' grids); the coat alone 0.93%,
        // a dielectric 1.55% and its specular alone 3.4% (0.0758 against 0.0733 at 60 degrees: its
        // exact Fresnel in F82's form, over step 1's EssD, itself 0.8% off at 18 degrees); everything
        // layered 1.7%. Each held with room for 2^19 samples' noise.
        for (float phi : {0.0f, 1.5707963f}) {
            Material brushed;
            brushed.baseMetalness = 1.0f;
            brushed.baseColor = {0.9f, 0.9f, 0.88f};
            brushed.specularRoughness = 0.4f;
            brushed.specularRoughnessAnisotropy = 0.6f;   // narrower: past the grid's 6 x 6 points a bin
            cases.push_back({brushed, mu, 0.01, phi});
            Material gold = brushed;   // a coloured metal: F82's moments matter most
            gold.baseColor = {0.95f, 0.64f, 0.24f};
            gold.specularColor = {0.9f, 0.8f, 0.6f};
            cases.push_back({gold, mu, 0.01, phi, false});
            Material satin;   // an anisotropic dielectric: its exact Fresnel in F82's form
            satin.baseColor = {0.5f, 0.1f, 0.1f};
            satin.specularRoughness = 0.4f;
            satin.specularRoughnessAnisotropy = 0.6f;
            cases.push_back({satin, mu, 0.025, phi, false});
            Material gloss = satin;   // its specular alone, over a black base
            gloss.baseColor = {0.0f, 0.0f, 0.0f};
            cases.push_back({gloss, mu, 0.05, phi, false});
            Material coat;   // an anisotropic coat alone over a white dielectric
            coat.coatWeight = 1.0f;
            coat.coatRoughness = 0.4f;
            coat.coatRoughnessAnisotropy = 0.6f;
            cases.push_back({coat, mu, 0.015, phi});
            Material all = car;   // everything at once, the coat anisotropic too
            all.specularRoughnessAnisotropy = 0.5f;
            all.coatRoughness = 0.4f;   // the grid's 6 x 6 points a bin hold alpha >= ~0.09 each way
            all.coatRoughnessAnisotropy = 0.5f;
            all.fuzzWeight = 0.5f;
            all.fuzzRoughness = 0.3f;
            cases.push_back({all, mu, 0.025, phi});
        }
    }
    return cases;
}

// sphere: mu's bins cover [-1, 1] (transmission), else [0, 1].
// The ops a sampling test draws, densities and weighs with: the whole BSDF (Sample, Pdf, Eval) or
// its smooth part (SampleSmooth, PdfSmooth, Parts' r0) at `cut`.
struct SamplingOps {
    Op sample{Op::Sample};
    Op pdf{Op::Pdf};
    Op eval{Op::Eval};
    float cut{0.25f};
};

void samplingChi2(const std::vector<Case>& cases, uint32_t seed, bool sphere = false, SamplingOps ops = {}) {
    // Over the sphere twice the polar bins, so each spans what a hemisphere's does.
    const unsigned kTheta = sphere ? 64u : 32u, kSub = sphere ? 12u : 6u;
    constexpr unsigned kPhi = 64, kN = 1u << 20;
    const float muLo = sphere ? -1.0f : 0.0f, muSpan = sphere ? 2.0f : 1.0f;
    const auto tested = std::count_if(cases.begin(), cases.end(), [](const Case& c) { return c.chi2; });
    const double threshold = 1.0 - std::pow(1.0 - 0.01, 1.0 / static_cast<double>(tested));   // Sidak
    for (const Case& c : cases) {
        if (!c.chi2) continue;
        // Expected counts: the pdf integrated over each bin (cos theta uniform bins, phi bins).
        std::vector<Item> grid;
        grid.reserve(kTheta * kPhi * kSub * kSub);
        for (unsigned t = 0; t < kTheta; ++t)
            for (unsigned p = 0; p < kPhi; ++p)
                for (unsigned i = 0; i < kSub; ++i)
                    for (unsigned j = 0; j < kSub; ++j) {
                        const float mu = muLo + muSpan * (t + (i + 0.5f) / kSub) / kTheta;
                        const float phi = static_cast<float>(2.0 * kPi) * (p + (j + 0.5f) / kSub) / kPhi - static_cast<float>(kPi);
                        const float s = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
                        Item it;
                        it.op = ops.pdf;
                        it.cut = ops.cut;
                        it.material = c.m;
                        it.wo = view(c.mu, c.phi);
                        it.v = {s * std::cos(phi), s * std::sin(phi), mu};
                        grid.push_back(it);
                    }
        ohao::material::run(grid, kTables);
        std::vector<double> expected(kTheta * kPhi, 0.0);
        const double cell = (muSpan / kTheta) * (2.0 * kPi / kPhi) / (kSub * kSub);
        for (size_t k = 0; k < grid.size(); ++k) expected[k / (kSub * kSub)] += grid[k].r0[0] * cell * kN;
        // Observed counts, and each sample's weight against eval / pdf.
        std::vector<Item> samples(kN);
        for (Item& it : samples) {
            it.op = ops.sample;
            it.cut = ops.cut;
            it.material = c.m;
            it.wo = view(c.mu, c.phi);
            it.v = {rnd(seed), rnd(seed), rnd(seed)};
        }
        ohao::material::run(samples, kTables);
        std::vector<double> observed(kTheta * kPhi, 0.0);
        std::vector<Item> checks;
        unsigned drawn = 0;
        for (const Item& it : samples) {
            if (!(it.r0[3] > 0.0f)) continue;
            ++drawn;
            const float mu = std::clamp(it.r0[2], muLo, 0.999999f);
            float phi = std::atan2(it.r0[1], it.r0[0]);
            const unsigned t = std::min(static_cast<unsigned>((mu - muLo) / muSpan * kTheta), kTheta - 1);
            const unsigned p = std::min(static_cast<unsigned>((phi + kPi) / (2.0 * kPi) * kPhi), kPhi - 1);
            observed[t * kPhi + p] += 1.0;
            if (checks.size() < 4096) {
                Item e;
                e.op = ops.eval;
                e.cut = ops.cut;
                e.material = c.m;
                e.wo = view(c.mu, c.phi);
                e.v = {it.r0[0], it.r0[1], it.r0[2]};
                e.r1 = {it.r0[3], it.r1[0], it.r1[1], it.r1[2]};   // pdf, weight
                checks.push_back(e);
            }
        }
        CHECK(drawn > kN / 4, ("the sampler draws (most samples have a density): " + describe(c.m, c.mu)).c_str());
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

void t1_sampling_chi2() { samplingChi2(lobeCases(), 11u); }

void albedoMatchesIntegral(const std::vector<Case>& cases, uint32_t seed, unsigned samples) {
    for (const Case& c : cases) {
        const Item a = one(Op::Albedo, c.m, view(c.mu, c.phi), {});
        const auto mc = meanWeight(c.m, view(c.mu, c.phi), samples, seed++);
        for (int ch = 0; ch < 3; ++ch) {
            const double rel = std::abs(a.r0[ch] - mc[ch]) / std::max(mc[ch], 1e-3);
            if (!(rel <= c.albedoTol))
                std::printf("\n    albedo %.5f vs integral %.5f (phi %.2f): %s", a.r0[ch], mc[ch], c.phi,
                            describe(c.m, c.mu).c_str());
            CHECK(rel <= c.albedoTol, ("albedo() is not the integral of eval(): " + describe(c.m, c.mu)).c_str());
        }
    }
}

void t1_albedo() { albedoMatchesIntegral(lobeCases(), 23u, 1u << 18); }

void t2_defaults() {
    const Material m;
    CHECK(m.coatWeight == 0.0f && m.coatColor[0] == 1.0f && m.coatRoughness == 0.0f, "coat weight, colour, roughness");
    CHECK(m.coatRoughnessAnisotropy == 0.0f && m.coatIor == 1.6f && m.coatDarkening == 1.0f, "coat anisotropy, ior, darkening");
    CHECK(m.fuzzWeight == 0.0f && m.fuzzColor[0] == 1.0f && m.fuzzRoughness == 0.5f, "fuzz");
    CHECK(m.specularRoughnessAnisotropy == 0.0f, "specular_roughness_anisotropy 0");
}

void t2_reciprocity() {
    uint32_t seed = 9u;
    for (int k = 0; k < 2000; ++k) {
        Material m;
        m.specularRoughness = 0.05f + 0.95f * rnd(seed);
        m.specularRoughnessAnisotropy = rnd(seed);
        m.coatRoughness = 0.05f + 0.95f * rnd(seed);
        m.coatRoughnessAnisotropy = rnd(seed);
        m.coatIor = 1.0f + 2.0f * rnd(seed);
        auto dir = [&] {
            const float mu = 0.02f + 0.98f * rnd(seed), phi = 6.2831853f * rnd(seed);
            const float s = std::sqrt(1.0f - mu * mu);
            return Float3{s * std::cos(phi), s * std::sin(phi), mu};
        };
        const Float3 a = dir(), b = dir();
        const Item ab = one(Op::Lobes, m, a, b), ba = one(Op::Lobes, m, b, a);
        CHECK(std::abs(ab.r0[0] - ba.r0[0]) <= 1e-4f * std::max(1.0f, std::abs(ab.r0[0])), "anisotropic GGX is not reciprocal");
        CHECK(std::abs(ab.r1[1] - ba.r1[1]) <= 1e-4f * std::max(1.0f, std::abs(ab.r1[1])), "the coat's lobe is not reciprocal");
    }
}

void t2_sampling_chi2() { samplingChi2(layeredCases(), 13u); }

void t2_albedo() { albedoMatchesIntegral(layeredCases(), 29u, 1u << 19); }

void t2_white_furnace() {
    uint32_t seed = 37u;
    for (float rough : {0.01f, 0.3f, 0.6f, 1.0f})
        for (float deg : {0.0f, 30.0f, 60.0f, 85.0f}) {
            const float mu = std::cos(deg * static_cast<float>(kPi) / 180.0f);
            Material white;
            white.baseColor = {1.0f, 1.0f, 1.0f};
            std::vector<Material> ms;
            for (float dark : {0.0f, 1.0f})
                for (float metal : {0.0f, 1.0f}) {
                    Material c = white;
                    c.baseMetalness = metal;
                    c.coatWeight = 1.0f;
                    c.coatRoughness = rough;
                    c.coatDarkening = dark;
                    ms.push_back(c);
                }
            Material half = white;   // half a coat
            half.coatWeight = 0.5f;
            half.coatRoughness = rough;
            ms.push_back(half);
            Material fuzz = white;
            fuzz.fuzzWeight = 1.0f;
            fuzz.fuzzRoughness = std::max(rough, 0.05f);
            ms.push_back(fuzz);
            for (const Material& m : ms) {
                const Item a = one(Op::Albedo, m, view(mu), {});
                const auto mc = meanWeight(m, view(mu), 1u << 18, seed++);
                if (!(std::abs(a.r0[0] - 1.0f) <= 0.01f && std::abs(mc[0] - 1.0) <= 0.01))
                    std::printf("\n    albedo %.5f integral %.5f: %s", a.r0[0], mc[0], describe(m, mu).c_str());
                CHECK(std::abs(a.r0[0] - 1.0f) <= 0.01f, ("albedo() is not 1: " + describe(m, mu)).c_str());
                CHECK(std::abs(mc[0] - 1.0) <= 0.01, ("the integral of eval() is not 1: " + describe(m, mu)).c_str());
            }
        }
}

void t2_aniso_furnace() {
    uint32_t seed = 41u;
    for (float aniso : {0.5f, 0.9f})
        for (float rough : {0.01f, 0.3f, 0.6f})
            for (float deg : {0.0f, 60.0f, 85.0f})
                for (float phi : {0.0f, 1.570796f}) {
                    const float mu = std::cos(deg * static_cast<float>(kPi) / 180.0f);
                    const float s = std::sqrt(1.0f - mu * mu);
                    const Float3 wo{s * std::cos(phi), s * std::sin(phi), mu};
                    Material m;
                    m.baseColor = {1.0f, 1.0f, 1.0f};
                    m.baseMetalness = 1.0f;
                    m.specularRoughness = rough;
                    m.specularRoughnessAnisotropy = aniso;
                    // At 85 degrees across the grain the isotropic compensation leaves up to 4.2% (roughness
                    // 0.6, anisotropy 0.9; a 4x finer AnisoRough left it), a known gap.
                    const double tol = deg > 60.0f ? 0.05 : 0.01;
                    const Item a = one(Op::Albedo, m, wo, {});
                    const auto mc = meanWeight(m, wo, 1u << 20, seed++);
                    if (!(std::abs(a.r0[0] - 1.0f) <= 0.01f && std::abs(mc[0] - 1.0) <= tol))
                        std::printf("\n    albedo %.5f integral %.5f phi %.2f: %s", a.r0[0], mc[0], phi, describe(m, mu).c_str());
                    CHECK(std::abs(a.r0[0] - 1.0f) <= 0.01f, ("albedo() is not 1: " + describe(m, mu)).c_str());
                    CHECK(std::abs(mc[0] - 1.0) <= tol, ("the integral of eval() is not 1: " + describe(m, mu)).c_str());
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

// ---- step 3: transmission, thin-walled, thin film, dispersion ----------------------------------

void t3_defaults() {
    const Material m;
    CHECK(m.transmissionWeight == 0.0f && m.transmissionColor[0] == 1.0f && !m.thinWalled, "transmission, thin-walled");
    CHECK(m.thinFilmWeight == 0.0f && m.thinFilmThickness == 0.5f && m.thinFilmIor == 1.4f, "thin film");
}

// Glass: solid (outside and inside), thin-walled, half transmissive, tinted, coated.
std::vector<Case> glassCases() {
    std::vector<Case> cases;
    for (float rough : {0.3f, 0.6f})
        for (float mu : {0.9f, 0.4f, -0.9f, -0.4f}) {
            Material glass;
            glass.transmissionWeight = 1.0f;
            glass.specularRoughness = rough;
            cases.push_back({glass, mu, 0.01});
            Material thin = glass;
            thin.thinWalled = true;
            cases.push_back({thin, mu, 0.01});
            if (mu < 0.0f) continue;   // the rest from outside
            Material half = glass;     // half refracts, half is the diffuse
            half.transmissionWeight = 0.5f;
            half.baseColor = {0.7f, 0.4f, 0.2f};
            half.transmissionColor = {0.9f, 0.6f, 0.3f};
            cases.push_back({half, mu, 0.01});
            Material coated = glass;
            coated.coatWeight = 1.0f;
            coated.coatRoughness = 0.3f;
            cases.push_back({coated, mu, 0.01});
        }
    return cases;
}

void t3_sampling_chi2() { samplingChi2(glassCases(), 43u, true); }

void t3_albedo() {
    std::vector<Case> outside;
    for (const Case& c : glassCases())
        if (c.mu > 0.0f) outside.push_back(c);
    albedoMatchesIntegral(outside, 47u, 1u << 19);
}

void t3_white_furnace() {
    uint32_t seed = 53u;
    for (float ior : {1.33f, 1.5f, 2.4f})
        for (float rough : {0.01f, 0.3f, 0.6f, 1.0f})
            for (float deg : {0.0f, 30.0f, 60.0f, 85.0f, 120.0f, 150.0f, 180.0f}) {
                const float mu = std::cos(deg * static_cast<float>(kPi) / 180.0f);
                Material glass;
                glass.baseColor = {1.0f, 1.0f, 1.0f};
                glass.transmissionWeight = 1.0f;
                glass.specularIor = ior;
                glass.specularRoughness = rough;
                std::vector<Material> ms{glass};
                Material thin = glass;
                thin.thinWalled = true;
                ms.push_back(thin);
                if (mu > 0.0f) {   // half diffuse, from outside
                    Material half = glass;
                    half.transmissionWeight = 0.5f;
                    ms.push_back(half);
                }
                for (const Material& m : ms) {
                    const auto mc = meanWeight(m, view(mu), 1u << 18, seed++);
                    if (!(std::abs(mc[0] - 1.0) <= 0.01))
                        std::printf("\n    integral %.5f: %s thin %d", mc[0], describe(m, mu).c_str(), m.thinWalled ? 1 : 0);
                    CHECK(std::abs(mc[0] - 1.0) <= 0.01, ("the integral of eval() is not 1: " + describe(m, mu)).c_str());
                    if (mu > 0.0f) {
                        const Item a = one(Op::Albedo, m, view(mu), {});
                        if (!(std::abs(a.r0[0] - 1.0f) <= 0.01f))
                            std::printf("\n    albedo %.5f: %s thin %d", a.r0[0], describe(m, mu).c_str(), m.thinWalled ? 1 : 0);
                        CHECK(std::abs(a.r0[0] - 1.0f) <= 0.01f, ("albedo() is not 1: " + describe(m, mu)).c_str());
                    }
                }
            }
}

void t3_thin_film() {
    for (float mu : {1.0f, 0.7f, 0.3f}) {
        for (float d : {0.0f, 150.0f, 300.0f, 600.0f, 1000.0f}) {
            const Item f = one(Op::Film, Material{}, {mu, 1.4f, d}, {0.04f, 0.04f, 0.04f});
            for (int c = 0; c < 3; ++c) CHECK(f.r0[c] >= 0.0f && f.r0[c] <= 1.0f, "the Airy reflectance in [0, 1]");
        }
        // No film: the base's Fresnel (Schlick's of its F0 in the fit) within 0.01.
        const Item none = one(Op::Film, Material{}, {mu, 1.4f, 0.0f}, {0.04f, 0.04f, 0.04f});
        const float m5 = std::pow(1.0f - mu, 5.0f), schlick = 0.04f + 0.96f * m5;
        CHECK(std::abs(none.r0[1] - schlick) <= 0.01f, "no thickness, no film");
    }
    const Item col = one(Op::Film, Material{}, {1.0f, 1.4f, 300.0f}, {0.04f, 0.04f, 0.04f});
    CHECK(std::abs(col.r0[0] - col.r0[2]) > 0.01f, "a 300 nm film colours the reflection");
    // A filmed white metal keeps its energy within 1% (the film recolours F; F stays near 1).
    Material metal;
    metal.baseMetalness = 1.0f;
    metal.baseColor = {1.0f, 1.0f, 1.0f};
    metal.thinFilmWeight = 1.0f;
    metal.thinFilmThickness = 0.3f;
    metal.specularRoughness = 0.3f;
    const auto mc = meanWeight(metal, view(0.8f), 1u << 18, 59u);
    for (int c = 0; c < 3; ++c) CHECK(mc[c] > 0.9 && mc[c] <= 1.01, "a filmed white metal stays near white");
}

void t3_dispersion() {
    const Item d = one(Op::Ior, Material{}, {1.5f, 20.0f, 1.0f}, {587.56f, 0.0f, 0.0f});
    CHECK(std::abs(d.r0[0] - 1.5f) < 1e-4f, "the d line's IOR is specular_ior");
    const Item blue = one(Op::Ior, Material{}, {1.5f, 20.0f, 1.0f}, {450.0f, 0.0f, 0.0f});
    const Item red = one(Op::Ior, Material{}, {1.5f, 20.0f, 1.0f}, {600.0f, 0.0f, 0.0f});
    CHECK(blue.r0[0] > 1.5f && red.r0[0] < 1.5f, "blue bends more than red");
    const Item none = one(Op::Ior, Material{}, {1.5f, 20.0f, 0.0f}, {450.0f, 0.0f, 0.0f});
    CHECK(none.r0[0] == 1.5f, "scale 0: no dispersion");
}

} // namespace


// ---- step 4: subsurface ---------------------------------------------------------------------

void t4_defaults() {
    const Material m;
    CHECK(m.subsurfaceWeight == 0.0f && m.subsurfaceColor[0] == 0.8f && !m.subsurfaceWalked, "subsurface defaults");
}

void t4_van_de_hulst() {
    const Item a = one(Op::VanDeHulst, Material{}, {0.0f, 0.5f, 1.0f}, {});
    CHECK(a.r0[0] < 1e-4f && a.r0[2] > 0.9999f, "black is 0, white 1");
    CHECK(a.r0[1] > 0.5f && a.r0[1] < 1.0f, "a grey volume scatters more than it shows (0.5 shows from ~0.97)");
}

std::vector<Case> subsurfaceCases(bool walked) {
    std::vector<Case> cases;
    for (float mu : {0.9f, 0.5f, 0.15f}) {
        Material skin;
        skin.baseColor = {0.5f, 0.5f, 0.5f};
        skin.subsurfaceWeight = 1.0f;
        skin.subsurfaceColor = {0.9f, 0.6f, 0.5f};
        skin.specularRoughness = 0.4f;
        skin.subsurfaceWalked = walked;
        cases.push_back({skin, mu, 0.01});
        Material half = skin;   // half subsurface, half diffuse, over a rough diffuse
        half.subsurfaceWeight = 0.5f;
        half.baseDiffuseRoughness = 0.5f;
        cases.push_back({half, mu, 0.01});
    }
    return cases;
}

void t4_local() {
    samplingChi2(subsurfaceCases(false), 61u);
    albedoMatchesIntegral(subsurfaceCases(false), 67u, 1u << 18);
    uint32_t seed = 71u;
    for (float rough : {0.05f, 0.5f, 1.0f})
        for (float mu : {1.0f, 0.5f, 0.1f}) {
            Material m;
            m.baseColor = {1.0f, 1.0f, 1.0f};
            m.subsurfaceWeight = 1.0f;
            m.subsurfaceColor = {1.0f, 1.0f, 1.0f};
            m.specularRoughness = rough;
            const auto mc = meanWeight(m, view(mu), 1u << 18, seed++);
            CHECK(std::abs(mc[0] - 1.0) <= 0.01, ("a white local subsurface is not white: " + describe(m, mu)).c_str());
        }
}

void t4_walked() {
    // What enters (lobe 5) and what the surface reflects add to white when the walk returns all of it.
    uint32_t seed = 73u;
    for (float rough : {0.05f, 0.5f, 1.0f})
        for (float mu : {1.0f, 0.5f, 0.1f})
            for (float weight : {1.0f, 0.5f}) {
                Material m;
                m.baseColor = {1.0f, 1.0f, 1.0f};
                m.subsurfaceWeight = weight;
                m.subsurfaceColor = {1.0f, 1.0f, 1.0f};
                m.specularRoughness = rough;
                m.subsurfaceWalked = true;
                std::vector<Item> items(1u << 18);
                for (Item& it : items) {
                    it.op = Op::Sample;
                    it.material = m;
                    it.wo = view(mu);
                    it.v = {rnd(seed), rnd(seed), rnd(seed)};
                }
                ohao::material::run(items, kTables);
                double sum = 0.0;
                bool inward = true;
                for (const Item& it : items) {
                    sum += it.r1[0];
                    if (static_cast<int>(it.r1[3]) == 5 && it.r0[3] > 0.0f) inward = inward && it.r0[2] < 0.0f;
                }
                const double mean = sum / items.size();
                if (!(std::abs(mean - 1.0) <= 0.01)) std::printf("\n    walked %.5f: %s", mean, describe(m, mu).c_str());
                CHECK(std::abs(mean - 1.0) <= 0.01, ("reflected + entered is not white: " + describe(m, mu)).c_str());
                CHECK(inward, "lobe 5 points into the surface");
            }
    // pdf() is a density of the directions above the surface only.
    Material m;
    m.subsurfaceWeight = 1.0f;
    m.subsurfaceWalked = true;
    const Item p = one(Op::Pdf, m, view(0.7f), {0.0f, 0.0f, -1.0f});
    CHECK(p.r0[0] == 0.0f, "no density below an opaque subsurface");
}


// ---- area lights by linearly transformed cosines ------------------------------------------------

Float3 uniformSphere(uint32_t& seed) {
    const float z = 1.0f - 2.0f * rnd(seed), phi = 6.2831853f * rnd(seed);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(phi), r * std::sin(phi), z};
}

std::vector<double> ltcDensities(const Material& m, Float3 wo, const std::vector<Float3>& dirs) {
    std::vector<Item> items(dirs.size());
    for (size_t k = 0; k < dirs.size(); ++k) {
        items[k].op = Op::LtcDensity;
        items[k].material = m;
        items[k].wo = wo;
        items[k].v = dirs[k];
    }
    ohao::material::run(items, kTables);
    std::vector<double> d(items.size());
    for (size_t k = 0; k < items.size(); ++k) d[k] = items[k].r0[0];
    return d;
}

void t5_ltc_normalized() {
    uint32_t seed = 81u;
    for (float rough : {0.3f, 0.6f, 1.0f})   // uniform directions resolve no narrower lobe
        for (float mu : {1.0f, 0.7f, 0.3f}) {
            Material m;
            m.specularRoughness = rough;
            std::vector<Float3> dirs(1u << 20);
            for (Float3& d : dirs) d = uniformSphere(seed);
            const auto dens = ltcDensities(m, view(mu), dirs);
            double sum = 0.0;
            for (double v : dens) sum += v;
            const double integral = sum * 4.0 * kPi / dirs.size();
            if (!(std::abs(integral - 1.0) <= 0.02)) std::printf("\n    ltc integral %.4f rough %.2f mu %.2f", integral, rough, mu);
            CHECK(std::abs(integral - 1.0) <= 0.02, "the LTC's density integrates to 1");
        }
}

void t5_ltc_fit() {
    uint32_t seed = 83u;
    double worst = 0.0;
    for (float rough : {0.2f, 0.4f, 0.7f, 1.0f})
        for (float mu : {1.0f, 0.7f, 0.4f}) {
            Material m;
            m.specularRoughness = rough;
            std::vector<Float3> dirs(1u << 18);
            for (Float3& d : dirs) {
                d = uniformSphere(seed);
                d[2] = std::abs(d[2]);   // the upper hemisphere, pdf 1 / (2 pi)
            }
            const auto dens = ltcDensities(m, view(mu), dirs);
            std::vector<Item> lobes(dirs.size());
            for (size_t k = 0; k < dirs.size(); ++k) {
                lobes[k].op = Op::Lobes;
                lobes[k].material = m;
                lobes[k].wo = view(mu);
                lobes[k].v = dirs[k];
            }
            ohao::material::run(lobes, kTables);
            double e = 0.0;
            for (size_t k = 0; k < dirs.size(); ++k) e += lobes[k].r0[0] * dirs[k][2];
            e *= 2.0 * kPi / dirs.size();   // the lobe's albedo, F = 1
            double l1 = 0.0;
            for (size_t k = 0; k < dirs.size(); ++k) l1 += std::abs(dens[k] - lobes[k].r0[0] * dirs[k][2] / e);
            l1 *= 2.0 * kPi / dirs.size();
            worst = std::max(worst, l1);
            if (!(l1 <= 0.25)) std::printf("\n    ltc L1 %.3f rough %.2f mu %.2f", l1, rough, mu);
            CHECK(l1 <= 0.25, "the LTC stays near the GGX lobe it was fitted to");
        }
    std::printf(" [worst L1 %.3f]", worst);
}

void t5_ltc_quad() {
    struct Q { float x0, y0, x1, y1, z; };
    // (No quad reaching the horizon all round: clipped, it makes an edge between opposite points,
    // where the edge integral's theta / sin(theta) has no precision left; no light makes one.)
    const Q quads[] = {{-3.0f, -3.0f, 3.0f, 3.0f, 1.0f},   // a wide quad overhead
                       {-0.3f, -0.2f, 0.4f, 0.5f, 1.0f},   // a small quad overhead
                       {0.5f, -1.0f, 3.0f, 1.0f, 1.0f},    // toward the reflection
                       {-2.0f, -2.0f, 2.0f, 2.0f, 0.05f}}; // almost in the plane
    uint32_t seed = 89u;
    for (float rough : {0.3f, 0.7f})
        for (float mu : {1.0f, 0.5f})
            for (const Q& q : quads) {
                Material m;
                m.specularRoughness = rough;
                m.coatWeight = q.x0;   // the quad's lower corner rides in the coat's fields (op 13)
                m.coatRoughness = q.y0;
                const Item a = one(Op::LtcQuad, m, view(mu), {q.x1, q.y1, q.z});
                // Monte Carlo: the density over the directions that meet the quad.
                std::vector<Float3> dirs(1u << 20);
                for (Float3& d : dirs) d = uniformSphere(seed);
                Material plain;
                plain.specularRoughness = rough;
                const auto dens = ltcDensities(plain, view(mu), dirs);
                double sum = 0.0;
                for (size_t k = 0; k < dirs.size(); ++k) {
                    const Float3& d = dirs[k];
                    if (!(d[2] > 0.0f)) continue;
                    const double t = q.z / d[2], x = d[0] * t, y = d[1] * t;
                    if (x >= q.x0 && x <= q.x1 && y >= q.y0 && y <= q.y1) sum += dens[k];
                }
                const double mc = sum * 4.0 * kPi / dirs.size();
                const bool ok = std::abs(a.r0[0] - mc) <= 0.01 + 0.02 * mc;
                if (!ok) std::printf("\n    quad %.4f mc %.4f rough %.2f mu %.2f z %.2f", a.r0[0], mc, rough, mu, q.z);
                CHECK(ok, "ltcQuad is the LTC's integral over the quad");
            }
}

// ---- caustics (sub-project 24, T1): the smooth and rough parts ----------------------------------

// Every family the module has, from both sides where it transmits.
std::vector<Case> partsCases() {
    std::vector<Case> cases;
    for (const Case& c : lobeCases()) cases.push_back(c);
    for (const Case& c : layeredCases()) cases.push_back(c);
    for (const Case& c : glassCases()) cases.push_back(c);
    Material plinth;   // the showcase's plinth: a rough base under a smooth coat
    plinth.baseColor = {0.82f, 0.8f, 0.76f};
    plinth.specularRoughness = 0.35f;
    plinth.coatWeight = 1.0f;
    plinth.coatRoughness = 0.04f;
    for (float mu : {0.95f, 0.5f, 0.15f}) cases.push_back({plinth, mu});
    Material mirror;
    mirror.baseMetalness = 1.0f;
    mirror.baseColor = {0.9f, 0.9f, 0.9f};
    mirror.specularRoughness = 0.0f;
    for (float mu : {0.95f, 0.5f}) cases.push_back({mirror, mu});
    return cases;
}

void t6_parts_sum() {
    uint32_t seed = 61u;
    for (const Case& c : partsCases())
        for (float cut : {0.0f, 0.1f, 0.25f, 1.01f}) {
            std::vector<Item> parts, evals;
            for (unsigned i = 0; i < 32; ++i)
                for (unsigned j = 0; j < 32; ++j) {
                    const float mu = -1.0f + 2.0f * (i + rnd(seed)) / 32.0f;
                    const float phi = static_cast<float>(2.0 * kPi) * (j + rnd(seed)) / 32.0f;
                    const float sn = std::sqrt(std::max(0.0f, 1.0f - mu * mu));
                    Item it;
                    it.op = Op::Parts;
                    it.cut = cut;
                    it.material = c.m;
                    it.wo = view(c.mu, c.phi);
                    it.v = {sn * std::cos(phi), sn * std::sin(phi), mu};
                    parts.push_back(it);
                    it.op = Op::Eval;
                    evals.push_back(it);
                }
            ohao::material::run(parts, kTables);
            ohao::material::run(evals, kTables);
            bool ok = true;
            for (size_t k = 0; k < parts.size(); ++k)
                for (int ch = 0; ch < 3; ++ch) {
                    const float sum = parts[k].r0[ch] + parts[k].r1[ch], want = evals[k].r0[ch];
                    if (!(std::abs(sum - want) <= 1e-5f * std::max(1e-3f, std::abs(want)))) ok = false;
                    if (!(parts[k].r0[ch] >= 0.0f && parts[k].r1[ch] >= 0.0f)) ok = false;
                }
            if (!ok) std::printf("\n    cut %.2f: %s", cut, describe(c.m, c.mu).c_str());
            CHECK(ok, "smooth + rough is eval(), each part non-negative");
        }
}

void t6_parts_threshold() {
    // A dielectric exactly at the cut is rough on both sides; just under it, its specular is smooth.
    const Float3 wo = view(0.7f), wi = {-0.5f, 0.1f, 0.86f};
    auto parts = [&](float rough) {
        Material m;
        m.specularRoughness = rough;
        Item it;
        it.op = Op::Parts;
        it.cut = 0.25f;
        it.material = m;
        it.wo = wo;
        it.v = wi;
        ohao::material::run(std::span<Item>(&it, 1), kTables);
        return it;
    };
    const Item at = parts(0.25f), below = parts(0.2499f);
    CHECK(at.r0[0] == 0.0f && at.r0[1] == 0.0f && at.r0[2] == 0.0f, "roughness == cut is all rough");
    CHECK(below.r0[0] > 0.0f, "roughness just under the cut has a smooth part");
    CHECK(below.r1[0] > 0.0f && below.r1[0] < at.r1[0], "under the cut the rough part keeps the diffuse alone");
}

std::vector<Case> smoothCases() {
    // Lobes the chi-square grid can integrate (6 x 6 points a bin hold roughness >= ~0.2; sharper
    // peaks fail the grid, not the sampler): a metal, glass from both sides, a coat over a rough base.
    std::vector<Case> cases;
    for (float mu : {0.9f, 0.4f}) {
        Material metal;
        metal.baseMetalness = 1.0f;
        metal.baseColor = {0.95f, 0.64f, 0.54f};
        metal.specularRoughness = 0.2f;
        cases.push_back({metal, mu});
        Material dielectric;   // the specular smooth, the diffuse rough
        dielectric.specularRoughness = 0.2f;
        cases.push_back({dielectric, mu});
        Material plinth;
        plinth.specularRoughness = 0.35f;
        plinth.coatWeight = 1.0f;
        plinth.coatRoughness = 0.2f;
        cases.push_back({plinth, mu});
    }
    for (float mu : {0.9f, 0.4f, -0.9f, -0.4f}) {
        Material glass;
        glass.transmissionWeight = 1.0f;
        glass.specularRoughness = 0.2f;
        cases.push_back({glass, mu});
        Material thin = glass;
        thin.thinWalled = true;
        cases.push_back({thin, mu});
    }
    return cases;
}

void t6_sample_smooth() { samplingChi2(smoothCases(), 67u, true, {Op::SampleSmooth, Op::PdfSmooth, Op::Parts, 0.25f}); }


int main(int argc, char** argv) {
    std::printf("material_tests (OpenPBR on the CPU)\n");
    // material_tests [NAME]: only the test NAME (t2_albedo); every test without one.
    const std::string only = argc > 1 ? argv[1] : "";
#define RUN_ONE(fn) \
    if (only.empty() || only == #fn) RUN(fn)
    RUN_ONE(t1_defaults);
    RUN_ONE(t1_fresnel);
    RUN_ONE(t1_weight_zero);
    RUN_ONE(t1_mirror_metal);
    RUN_ONE(t1_reciprocity);
    RUN_ONE(t1_sampling_chi2);
    RUN_ONE(t1_albedo);
    RUN_ONE(t1_white_furnace);
    RUN_ONE(t2_defaults);
    RUN_ONE(t2_reciprocity);
    RUN_ONE(t2_sampling_chi2);
    RUN_ONE(t2_albedo);
    RUN_ONE(t2_white_furnace);
    RUN_ONE(t2_aniso_furnace);
    RUN_ONE(t3_defaults);
    RUN_ONE(t3_sampling_chi2);
    RUN_ONE(t3_albedo);
    RUN_ONE(t3_white_furnace);
    RUN_ONE(t3_thin_film);
    RUN_ONE(t3_dispersion);
    RUN_ONE(t4_defaults);
    RUN_ONE(t4_van_de_hulst);
    RUN_ONE(t4_local);
    RUN_ONE(t4_walked);
    RUN_ONE(t5_ltc_normalized);
    RUN_ONE(t5_ltc_fit);
    RUN_ONE(t5_ltc_quad);
    RUN_ONE(t6_parts_sum);
    RUN_ONE(t6_parts_threshold);
    RUN_ONE(t6_sample_smooth);
    return ohao::test::finish("material_tests");
}
