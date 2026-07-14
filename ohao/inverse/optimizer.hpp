#pragma once

// Optimizers for inverse rendering (Phase A).

#include "inverse/param_space.hpp"

#include <cmath>
#include <vector>

namespace ohao::inverse {

struct AdamState {
    std::vector<double> m;
    std::vector<double> v;
    int t{0};
    double beta1{0.9};
    double beta2{0.999};
    double eps{1e-8};

    void resize(size_t n) {
        m.assign(n, 0.0);
        v.assign(n, 0.0);
        t = 0;
    }

    void step(ParamSpace& space, const std::vector<double>& grad, double lr) {
        if (m.size() != space.size()) resize(space.size());
        ++t;
        const double b1t = 1.0 - std::pow(beta1, t);
        const double b2t = 1.0 - std::pow(beta2, t);
        for (size_t i = 0; i < space.size(); ++i) {
            m[i] = beta1 * m[i] + (1.0 - beta1) * grad[i];
            v[i] = beta2 * v[i] + (1.0 - beta2) * grad[i] * grad[i];
            const double mhat = m[i] / b1t;
            const double vhat = v[i] / b2t;
            space.values[i] =
                space.project(i, space.values[i] - lr * mhat / (std::sqrt(vhat) + eps));
        }
    }
};

} // namespace ohao::inverse
