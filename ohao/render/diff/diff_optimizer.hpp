#pragma once

// Adam on Diff albedo map floats.

#include "render/diff/diff_map.hpp"

#include <cmath>
#include <vector>

namespace ohao::diff {

struct DiffAdam {
    std::vector<float> m;
    std::vector<float> v;
    int t{0};
    float beta1{0.9f};
    float beta2{0.999f};
    float eps{1e-8f};

    void resize(size_t n) {
        m.assign(n, 0.f);
        v.assign(n, 0.f);
        t = 0;
    }

    void step(DiffAlbedoMap& map, const std::vector<float>& grad, float lr) {
        if (m.size() != map.rgb.size()) resize(map.rgb.size());
        ++t;
        const float b1t = 1.f - std::pow(beta1, static_cast<float>(t));
        const float b2t = 1.f - std::pow(beta2, static_cast<float>(t));
        for (size_t i = 0; i < map.rgb.size(); ++i) {
            const float g = (i < grad.size()) ? grad[i] : 0.f;
            m[i] = beta1 * m[i] + (1.f - beta1) * g;
            v[i] = beta2 * v[i] + (1.f - beta2) * g * g;
            const float mhat = m[i] / b1t;
            const float vhat = v[i] / b2t;
            map.rgb[i] -= lr * mhat / (std::sqrt(vhat) + eps);
        }
        map.clamp01();
    }
};

} // namespace ohao::diff
