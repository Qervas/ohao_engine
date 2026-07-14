#pragma once

// Flattened optimizable parameters with box constraints (Phase 1 inverse).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace ohao::inverse {

struct ParamSpace {
    std::vector<double> values;   // current θ
    std::vector<double> lo;       // inclusive lower bounds
    std::vector<double> hi;       // inclusive upper bounds
    std::vector<std::string> names;

    [[nodiscard]] size_t size() const noexcept { return values.size(); }

    void add(std::string name, double v, double low, double high) {
        names.push_back(std::move(name));
        values.push_back(v);
        lo.push_back(low);
        hi.push_back(high);
    }

    void clampAll() {
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = std::clamp(values[i], lo[i], hi[i]);
        }
    }

    [[nodiscard]] double project(size_t i, double v) const {
        return std::clamp(v, lo[i], hi[i]);
    }

    /// L2 distance to another parameter vector (same size).
    [[nodiscard]] double l2To(const std::vector<double>& other) const {
        if (other.size() != values.size()) return std::numeric_limits<double>::infinity();
        double s = 0.0;
        for (size_t i = 0; i < values.size(); ++i) {
            const double d = values[i] - other[i];
            s += d * d;
        }
        return std::sqrt(s);
    }
};

/// Central-difference gradient: g_i = (L(θ+ε e_i) - L(θ-ε e_i)) / (2ε)
/// `lossAt` evaluates loss after applying the full parameter vector.
template<typename LossFn>
std::vector<double> finiteDiffGradient(const ParamSpace& space, double eps, LossFn&& lossAt) {
    std::vector<double> g(space.size(), 0.0);
    std::vector<double> theta = space.values;
    for (size_t i = 0; i < space.size(); ++i) {
        const double v0 = theta[i];
        const double hi = space.project(i, v0 + eps);
        const double lo = space.project(i, v0 - eps);
        const double denom = hi - lo;
        if (denom < 1e-12) {
            g[i] = 0.0;
            continue;
        }
        theta[i] = hi;
        const double Lh = lossAt(theta);
        theta[i] = lo;
        const double Ll = lossAt(theta);
        theta[i] = v0;
        g[i] = (Lh - Ll) / denom;
    }
    return g;
}

/// One projected gradient-descent step: θ ← clamp(θ − lr · g)
inline void gdStep(ParamSpace& space, const std::vector<double>& grad, double lr) {
    for (size_t i = 0; i < space.size(); ++i) {
        space.values[i] = space.project(i, space.values[i] - lr * grad[i]);
    }
}

} // namespace ohao::inverse
