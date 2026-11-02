// Path guiding's lobes (sub-project 23, test g1): shaders/sampling/vmf.slang run on the CPU through
// its slangc C++ twin (tools/sampling/vmf_kernel.slang), so these are the shader's own numbers.
#include <gtest/gtest.h>

#include "vmf_cpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace ohao::sampling;

namespace {

constexpr double kPi = 3.14159265358979323846;

Vec3 normalized(Vec3 v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The density at cosine w from the lobe's mean (a direction at that cosine about mu).
double pdfAtCos(const Vec3& mu, float kappa, double w) {
    // A direction with dot(dir, mu) = w: rotate mu toward any perpendicular.
    const Vec3 p = std::abs(mu[2]) < 0.9f ? normalized({-mu[1], mu[0], 0.0f}) : normalized({0.0f, -mu[2], mu[1]});
    const double s = std::sqrt(std::max(0.0, 1.0 - w * w));
    const Vec3 d = {float(w * mu[0] + s * p[0]), float(w * mu[1] + s * p[1]), float(w * mu[2] + s * p[2])};
    return vmfPdf(d, mu, kappa);
}

// The probability of w in [a, b]: 2 pi times the density's integral over w (it depends on w alone),
// by the midpoint rule on at least `steps` steps, and fine enough that a step is 1/50 of the lobe's
// width in w (1/kappa).
double massBetween(const Vec3& mu, float kappa, double a, double b, int steps) {
    steps = std::max(steps, int(std::min((b - a) * double(kappa) * 50.0, 4.0e6)));
    double sum = 0.0;
    const double h = (b - a) / steps;
    for (int i = 0; i < steps; ++i) sum += pdfAtCos(mu, kappa, a + (i + 0.5) * h);
    return 2.0 * kPi * sum * h;
}

// The cosine below which a lobe holds probability p (the vMF's closed-form CDF in w).
double quantile(double kappa, double p) {
    if (kappa < 1e-4) return -1.0 + 2.0 * p;
    return 1.0 + std::log(p + (1.0 - p) * std::exp(-2.0 * kappa)) / kappa;
}

// Wilson-Hilferty: the upper tail of chi-square with k degrees of freedom at x.
double chiSquareTail(double x, double k) {
    const double z = (std::cbrt(x / k) - (1.0 - 2.0 / (9.0 * k))) / std::sqrt(2.0 / (9.0 * k));
    return 0.5 * std::erfc(z / std::sqrt(2.0));
}

const float kKappas[] = {0.1f, 5.0f, 100.0f, 1.0e4f};

} // namespace

// The density integrates to 1 over the sphere at every concentration.
TEST(Vmf, g1_PdfIntegratesToOne) {
    const Vec3 mu = normalized({0.3f, -0.5f, 0.8f});
    for (float kappa : kKappas) {
        // Fine where the lobe is: split [-1, 1] at the cosine holding all but 1e-6 of it.
        const double split = std::clamp(quantile(kappa, 1e-6), -1.0, 1.0 - 1e-9);
        const double total = massBetween(mu, kappa, -1.0, split, 20000) + massBetween(mu, kappa, split, 1.0, 200000);
        EXPECT_NEAR(total, 1.0, 1e-3) << "kappa " << kappa;
    }
}

// Directions drawn by vmfSample have the density vmfPdf: chi-square over 64 cosine bins of equal
// probability (from the closed-form CDF) by 64 azimuth bins, each bin's expectation integrated from
// vmfPdf itself.
TEST(Vmf, g1_SampleMatchesPdf) {
    const Vec3 mu = normalized({0.3f, -0.5f, 0.8f});
    const Vec3 p = normalized({-mu[1], mu[0], 0.0f});
    const Vec3 q = {mu[1] * p[2] - mu[2] * p[1], mu[2] * p[0] - mu[0] * p[2], mu[0] * p[1] - mu[1] * p[0]};
    constexpr int kW = 64, kPhi = 64, kN = 400000;
    for (float kappa : kKappas) {
        std::vector<double> edges(kW + 1);
        for (int i = 0; i <= kW; ++i) edges[i] = std::clamp(quantile(kappa, double(i) / kW), -1.0, 1.0);
        edges[0] = -1.0;
        edges[kW] = 1.0;
        std::vector<double> expectW(kW);
        for (int i = 0; i < kW; ++i) expectW[i] = massBetween(mu, kappa, edges[i], edges[i + 1], 4000);
        std::vector<int> counts(kW * kPhi, 0);
        std::mt19937 rng(1234u);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int s = 0; s < kN; ++s) {
            const Vec3 d = vmfSample(mu, kappa, u(rng), u(rng));
            // The cosine in double from the float direction (1 - |d - mu|^2 / 2 for unit vectors): a
            // float dot product cannot resolve kappa 10^4's bins near 1.
            const double dl = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
            double e2 = 0.0;
            for (int c = 0; c < 3; ++c) e2 += (d[c] / dl - mu[c]) * (d[c] / dl - mu[c]);
            const double w = std::clamp(1.0 - 0.5 * e2, -1.0, 1.0);
            double phi = std::atan2(double(dot(d, q)), double(dot(d, p)));
            if (phi < 0.0) phi += 2.0 * kPi;
            const int bw = int(std::upper_bound(edges.begin(), edges.end(), w) - edges.begin()) - 1;
            const int bp = std::min(int(phi / (2.0 * kPi) * kPhi), kPhi - 1);
            ++counts[std::clamp(bw, 0, kW - 1) * kPhi + bp];
        }
        double chi2 = 0.0;
        int dof = -1;
        for (int i = 0; i < kW; ++i)
            for (int j = 0; j < kPhi; ++j) {
                const double e = expectW[i] / kPhi * kN;
                if (e < 5.0) continue;
                const double d = counts[i * kPhi + j] - e;
                chi2 += d * d / e;
                ++dof;
            }
        ASSERT_GT(dof, 1000) << "kappa " << kappa;
        // Four concentrations: Sidak's correction of a 1% test.
        EXPECT_GT(chiSquareTail(chi2, dof), 1.0 - std::pow(0.99, 0.25)) << "kappa " << kappa << " chi2 " << chi2
                                                                        << " dof " << dof;
    }
}

// The mean cosine of drawn samples is coth(kappa) - 1/kappa, and vmfKappa inverts it.
TEST(Vmf, g1_MeanCosineAndFit) {
    const Vec3 mu = normalized({0.0f, 1.0f, 0.2f});
    std::mt19937 rng(77u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (float kappa : kKappas) {
        double sum = 0.0;
        constexpr int kN = 200000;
        for (int s = 0; s < kN; ++s) sum += dot(vmfSample(mu, kappa, u(rng), u(rng)), mu);
        const double mean = sum / kN;
        const double expect = kappa < 1e-3 ? kappa / 3.0 : 1.0 / std::tanh(double(kappa)) - 1.0 / kappa;
        EXPECT_NEAR(mean, expect, 4e-3 + 1e-3 * expect) << "kappa " << kappa;
        EXPECT_NEAR(vmfMeanCosine(kappa), expect, 1e-4 + 1e-5 * expect) << "kappa " << kappa;
    }
    // The fit: over mean cosines 0.05 to 0.999, within 1% of the concentration that has it.
    double worst = 0.0;
    for (int i = 0; i <= 200; ++i) {
        const double r = 0.05 + (0.999 - 0.05) * i / 200.0;
        double lo = 1e-6, hi = 1e7;
        for (int it = 0; it < 200; ++it) {
            const double m = std::sqrt(lo * hi);
            ((1.0 / std::tanh(m) - 1.0 / m) < r ? lo : hi) = m;
        }
        worst = std::max(worst, std::abs(vmfKappa(float(r)) / lo - 1.0));
    }
    EXPECT_LT(worst, 0.01) << "worst relative error of vmfKappa";
}

// A state fed samples of a known lobe (unit luminance, points 10 m along the drawn directions)
// learns its mean and concentration. The blend (alpha 0.02 once full) averages about 99 samples,
// so its mean direction is off by about sqrt(2 / kappa) / sqrt(99) radians: held to 3.5 times that,
// and the concentration to 15%.
TEST(Vmf, g1_StateLearnsALobe) {
    const Vec3 x = {1.0f, 2.0f, 3.0f};
    const Vec3 mu0 = normalized({0.6f, 0.7f, -0.2f});
    std::mt19937 rng(9u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (float kappa0 : {5.0f, 50.0f, 500.0f}) {
        GuideStateCpu s;
        for (uint32_t i = 0; i < 4000; ++i) {
            const Vec3 d = vmfSample(mu0, kappa0, u(rng), u(rng));
            const Vec3 target = {x[0] + 10.0f * d[0], x[1] + 10.0f * d[1], x[2] + 10.0f * d[2]};
            s = guideUpdate(s, x, target, 1.0f, i);
        }
        Vec3 mu;
        float kappa = 0.0f;
        ASSERT_TRUE(guideLobe(s, x, mu, kappa));
        const double angle = std::acos(std::clamp(double(dot(mu, mu0)), -1.0, 1.0)) * 180.0 / kPi;
        const double bar = 3.5 * std::sqrt(2.0 / kappa0) / std::sqrt(99.0) * 180.0 / kPi;
        EXPECT_LT(angle, bar) << "kappa " << kappa0;
        EXPECT_NEAR(kappa / kappa0, 1.0, 0.15) << "kappa " << kappa0 << " learned " << kappa;
        EXPECT_EQ(s.nt & 0xFFFFu, 64u);
        EXPECT_EQ(s.nt >> 16u, 3999u & 0xFFFFu);
    }
}

// An empty state has no lobe; its first sample points the lobe at that sample.
TEST(Vmf, g1_EmptyAndFirstSample) {
    const Vec3 x = {0.0f, 0.0f, 0.0f};
    Vec3 mu;
    float kappa = 0.0f;
    EXPECT_FALSE(guideLobe(GuideStateCpu{}, x, mu, kappa));
    const GuideStateCpu s = guideUpdate(GuideStateCpu{}, x, {0.0f, 0.0f, 5.0f}, 2.0f, 7);
    EXPECT_FLOAT_EQ(s.w, 2.0f);
    ASSERT_TRUE(guideLobe(s, x, mu, kappa));
    EXPECT_NEAR(mu[2], 1.0f, 1e-6f);
    EXPECT_GT(kappa, 10.0f);   // one sample at 5 m: a sharp lobe (the prior weighs 1/25)
}
