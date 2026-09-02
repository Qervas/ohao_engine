#include "diff/geom/camera_projection.hpp"

#include <cmath>

namespace ohao::diff {

namespace {

std::array<double, 3> cross(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double length(const std::array<double, 3>& a) {
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

bool normalise(std::array<double, 3>& a) {
    const double len = length(a);
    if (!(len > 1e-12) || !std::isfinite(len)) return false;
    a[0] /= len;
    a[1] /= len;
    a[2] /= len;
    return true;
}

}  // namespace

bool PinholeProjection::setLookAt(const std::array<float, 3>& eye,
                                  const std::array<float, 3>& target,
                                  const std::array<float, 3>& upHint) {
    m_hasBasis = false;
    const std::array<double, 3> e = {eye[0], eye[1], eye[2]};
    // `back` points from the target towards the eye, so the camera looks
    // down -back and a point in front has a NEGATIVE camera z.
    std::array<double, 3> back = {e[0] - target[0], e[1] - target[1], e[2] - target[2]};
    if (!normalise(back)) return false;
    const std::array<double, 3> up = {upHint[0], upHint[1], upHint[2]};
    std::array<double, 3> right = cross(up, back);
    // Fails when the up hint is parallel to the view direction: the cross
    // product is then zero and there is no basis to build, which is a
    // caller error rather than something to paper over with a fallback axis.
    if (!normalise(right)) return false;
    std::array<double, 3> trueUp = cross(back, right);
    if (!normalise(trueUp)) return false;

    m_eye = e;
    m_r = {right[0],  right[1],  right[2],  trueUp[0], trueUp[1],
           trueUp[2], back[0],   back[1],   back[2]};
    m_hasBasis = true;
    return true;
}

bool PinholeProjection::setIntrinsics(float fx, float fy, float cx, float cy) {
    m_hasIntrinsics = false;
    if (!(std::fabs(fx) > 1e-9f) || !(std::fabs(fy) > 1e-9f)) return false;
    if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(cx) || !std::isfinite(cy)) {
        return false;
    }
    m_fx = fx;
    m_fy = fy;
    m_cx = cx;
    m_cy = cy;
    m_hasIntrinsics = true;
    return true;
}

std::array<double, 3> PinholeProjection::toCamera(double wx, double wy, double wz) const {
    const double dx = wx - m_eye[0];
    const double dy = wy - m_eye[1];
    const double dz = wz - m_eye[2];
    return {m_r[0] * dx + m_r[1] * dy + m_r[2] * dz, m_r[3] * dx + m_r[4] * dy + m_r[5] * dz,
            m_r[6] * dx + m_r[7] * dy + m_r[8] * dz};
}

bool PinholeProjection::project(const std::vector<float>& worldPositions,
                                std::vector<float>& outScreen) const {
    outScreen.clear();
    if (!valid() || worldPositions.empty() || worldPositions.size() % 3u != 0u) return false;
    const std::size_t vertices = worldPositions.size() / 3u;
    outScreen.resize(vertices * 2u, 0.0f);
    for (std::size_t v = 0; v < vertices; ++v) {
        const auto p = toCamera(worldPositions[v * 3u + 0u], worldPositions[v * 3u + 1u],
                                worldPositions[v * 3u + 2u]);
        // In front means z_c <= -kNearMargin. Refusing here rather than
        // clamping: a clamped projection is a large finite number, and a
        // large finite number is something an optimiser will descend.
        if (!(p[2] <= -kNearMargin)) {
            outScreen.clear();
            return false;
        }
        outScreen[v * 2u + 0u] = static_cast<float>(-m_fx * p[0] / p[2] + m_cx);
        outScreen[v * 2u + 1u] = static_cast<float>(-m_fy * p[1] / p[2] + m_cy);
    }
    return true;
}

std::vector<double> PinholeProjection::jacobian(double wx, double wy, double wz) const {
    if (!valid()) return {};
    const auto p = toCamera(wx, wy, wz);
    if (!(p[2] <= -kNearMargin)) return {};
    const double invZ = 1.0 / p[2];
    const double invZ2 = invZ * invZ;
    // d(u,v)/d(camera), from u = -fx x/z + cx and v = -fy y/z + cy.
    const double du[3] = {-m_fx * invZ, 0.0, m_fx * p[0] * invZ2};
    const double dv[3] = {0.0, -m_fy * invZ, m_fy * p[1] * invZ2};
    // ... then times R, because p_c = R (p_w - eye) and d(p_c)/d(p_w) = R.
    std::vector<double> j(6, 0.0);
    for (int col = 0; col < 3; ++col) {
        double su = 0.0;
        double sv = 0.0;
        for (int k = 0; k < 3; ++k) {
            su += du[k] * m_r[static_cast<std::size_t>(k) * 3u + static_cast<std::size_t>(col)];
            sv += dv[k] * m_r[static_cast<std::size_t>(k) * 3u + static_cast<std::size_t>(col)];
        }
        j[static_cast<std::size_t>(col)] = su;
        j[3u + static_cast<std::size_t>(col)] = sv;
    }
    return j;
}

std::vector<float> PinholeProjection::pullback(const std::vector<float>& worldPositions,
                                               const std::vector<float>& screenGradients) const {
    if (!valid() || worldPositions.empty() || worldPositions.size() % 3u != 0u) return {};
    const std::size_t vertices = worldPositions.size() / 3u;
    if (screenGradients.size() != vertices * 2u) return {};

    std::vector<float> out(vertices * 3u, 0.0f);
    for (std::size_t v = 0; v < vertices; ++v) {
        const std::vector<double> j = jacobian(worldPositions[v * 3u + 0u],
                                               worldPositions[v * 3u + 1u],
                                               worldPositions[v * 3u + 2u]);
        // One vertex behind the near plane makes the whole pullback
        // meaningless, not just its own three components -- the shape it
        // belongs to has no projection.
        if (j.size() != 6u) return {};
        const double gu = screenGradients[v * 2u + 0u];
        const double gv = screenGradients[v * 2u + 1u];
        for (std::size_t c = 0; c < 3u; ++c) {
            out[v * 3u + c] = static_cast<float>(gu * j[c] + gv * j[3u + c]);
        }
    }
    return out;
}

}  // namespace ohao::diff
