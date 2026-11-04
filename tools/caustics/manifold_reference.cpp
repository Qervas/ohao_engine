// m2's reference (docs/plans/2026-11-04-caustics-step2a.md, T6): the irradiance a vertical glossy mirror (white metal,
// OpenPBR through its CPU twin, tools/material) gives a horizontal floor from a sun at 45 degrees, per unit of the sun's
// irradiance, the mirror large against its lobe (check_caustics' m2 room). In the mirror's frame (z its normal, y up)
// the sun is at wi = (0, cos 45, sin 45), and a direction wo leaving the mirror reaches the floor (normal +y) at cosine
// -wo.y, so
//     E_floor / E = integral over wo of eval(wo, wi) max(0, -wo.y) dwo,
// eval the BSDF times cos(wi). Integrated over the half vector m by its slope sigma = (m.x, m.y) / m.z on a log-polar
// grid out to 10^4 alpha (dwo = 4 |wi.m| cos^3(theta_m) dsigma), split at the core |sigma| <= max(sqrt(slope2) alpha,
// sqrt(minSlope2)) (an isotropic lobe: material/openpbr.slang's inCore).
//   manifold_reference ROUGHNESS [SLOPE2 [MINSLOPE2]]  ->  {"roughness", "total", "core", "tail"}
#include "openpbr_cpu.hpp"
#include "render/material/openpbr_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

using namespace ohao::material;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: manifold_reference ROUGHNESS [SLOPE2]\n");
        return 2;
    }
    const double rough = std::atof(argv[1]);
    const double slope2 = argc > 2 ? std::atof(argv[2]) : 99.0;
    const double alpha = std::max(rough, 0.01) * std::max(rough, 0.01);
    const double kPi = 3.14159265358979323846;
    const double c45 = std::sqrt(0.5);
    const double wi[3] = {0.0, c45, c45};
    Material metal;
    metal.baseColor = {1.0f, 1.0f, 1.0f};
    metal.baseMetalness = 1.0f;
    metal.specularRoughness = static_cast<float>(rough);
    const std::span<const float> tables(ohao::render::kOpenPBRTables, ohao::render::kOpenPBRTableFloats);

    const int nr = 4000, nphi = 720;
    const double rMin = 1e-4 * alpha, rMax = 1e4 * alpha;
    const double minSlope2 = argc > 3 ? std::atof(argv[3]) : 0.01;
    const double coreR = std::max(std::sqrt(slope2) * alpha, std::sqrt(minSlope2));
    std::vector<Item> items;
    std::vector<double> weights;
    std::vector<char> inCore;
    items.reserve(size_t(nr) * nphi);
    // The disc |sigma| < rMin: its integrand is the peak's, its share tiny; the midpoint at sigma = 0 covers it.
    for (int k = -1; k < nr; ++k) {
        double r, dr;
        if (k < 0) {
            r = 0.0;
            dr = rMin;
        } else {
            const double a = rMin * std::pow(rMax / rMin, double(k) / nr), b = rMin * std::pow(rMax / rMin, double(k + 1) / nr);
            r = std::sqrt(a * b);
            dr = b - a;
        }
        for (int j = 0; j < (k < 0 ? 1 : nphi); ++j) {
            const double phi = 2.0 * kPi * (j + 0.5) / nphi;
            const double sx = r * std::cos(phi), sy = r * std::sin(phi);
            const double len = std::sqrt(sx * sx + sy * sy + 1.0);
            const double m[3] = {sx / len, sy / len, 1.0 / len};
            const double c = wi[0] * m[0] + wi[1] * m[1] + wi[2] * m[2];
            if (c <= 0.0) continue;
            const double wo[3] = {2 * c * m[0] - wi[0], 2 * c * m[1] - wi[1], 2 * c * m[2] - wi[2]};
            if (wo[2] <= 0.0 || -wo[1] <= 0.0) continue;
            const double area = k < 0 ? kPi * rMin * rMin : r * dr * (2.0 * kPi / nphi);
            const double cos3 = 1.0 / (len * len * len);
            Item it;
            it.op = Op::Eval;
            it.material = metal;
            it.wo = {float(wo[0]), float(wo[1]), float(wo[2])};
            it.v = {float(wi[0]), float(wi[1]), float(wi[2])};
            items.push_back(it);
            weights.push_back(4.0 * c * cos3 * area * (-wo[1]));
            inCore.push_back(r <= coreR ? 1 : 0);
        }
    }
    run(items, tables);
    double total = 0.0, core = 0.0;
    for (size_t i = 0; i < items.size(); ++i) {
        const double v = double(items[i].r0[0]) * weights[i];
        total += v;
        if (inCore[i]) core += v;
    }
    std::printf("{\"roughness\": %.6g, \"slope2\": %.6g, \"total\": %.8g, \"core\": %.8g, \"tail\": %.8g}\n", rough, slope2,
                total, core, total - core);
    return 0;
}
