// The physical sky on the CPU (atmosphere.hpp): Hillaire 2020's tables and ray march.

#include "render/sky/atmosphere.hpp"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>

namespace ohao::render {

namespace {

constexpr float kPi = 3.14159265358979f;

float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// The distance from radius r along direction cosine mu to the sphere of radius R (the far side).
float distToSphere(float r, float mu, float R) {
    const float disc = r * r * (mu * mu - 1.0f) + R * R;
    return std::max(-r * mu + std::sqrt(std::max(disc, 0.0f)), 0.0f);
}

bool hitsGround(float r, float mu, float bottom) {
    return mu < 0.0f && r * r * (mu * mu - 1.0f) + bottom * bottom >= 0.0f;
}

float distToGround(float r, float mu, float bottom) {
    return std::max(-r * mu - std::sqrt(std::max(r * r * (mu * mu - 1.0f) + bottom * bottom, 0.0f)), 0.0f);
}

float rayleighPhase(float nu) { return 3.0f / (16.0f * kPi) * (1.0f + nu * nu); }

// Cornette-Shanks.
float miePhase(float nu, float g) {
    const float g2 = g * g;
    const float k = 3.0f / (8.0f * kPi) * (1.0f - g2) / (2.0f + g2);
    return k * (1.0f + nu * nu) / std::pow(std::max(1.0f + g2 - 2.0f * g * nu, 1e-6f), 1.5f);
}

constexpr float kIsotropicPhase = 1.0f / (4.0f * kPi);

glm::vec3 bilinear(const std::vector<glm::vec3>& t, int w, int h, float x, float y) {
    x = clamp01(x) * float(w - 1);
    y = clamp01(y) * float(h - 1);
    const int x0 = std::min(int(x), w - 2), y0 = std::min(int(y), h - 2);
    const float fx = x - float(x0), fy = y - float(y0);
    const glm::vec3 a = glm::mix(t[y0 * w + x0], t[y0 * w + x0 + 1], fx);
    const glm::vec3 b = glm::mix(t[(y0 + 1) * w + x0], t[(y0 + 1) * w + x0 + 1], fx);
    return glm::mix(a, b, fy);
}

}  // namespace

Atmosphere::Atmosphere(const AtmosphereParams& params) : m_p(params) {
    buildTransmittance();
    buildMultipleScattering();
}

Atmosphere::Medium Atmosphere::mediumAt(float altitude) const {
    altitude = std::max(altitude, 0.0f);
    const float rd = std::exp(-altitude / m_p.rayleighScaleHeight);
    const float md = std::exp(-altitude / m_p.mieScaleHeight);
    const float od = std::max(0.0f, 1.0f - std::abs(altitude - 25000.0f) / 15000.0f);
    Medium m;
    m.scatteringRayleigh = m_p.rayleighScattering * rd;
    m.scatteringMie = m_p.mieScattering * md;
    m.extinction = m.scatteringRayleigh + glm::vec3(m_p.mieExtinction * md) + m_p.ozoneAbsorption * od;
    return m;
}

glm::vec3 Atmosphere::transmittanceIntegral(float r, float mu) const {
    constexpr int kSteps = 500;
    const float d = distToSphere(r, mu, m_p.topRadius);
    const float dt = d / float(kSteps);
    glm::vec3 depth(0.0f);
    for (int i = 0; i < kSteps; ++i) {
        const float t = (float(i) + 0.5f) * dt;
        const float ri = std::sqrt(r * r + t * t + 2.0f * r * mu * t);
        depth += mediumAt(ri - m_p.bottomRadius).extinction * dt;
    }
    return glm::exp(-depth);
}

// Bruneton's parametrisation: x_r the horizon distance over its maximum, x_mu the distance to
// the top between its minimum and maximum at that radius.
void Atmosphere::buildTransmittance() {
    m_transmittance.assign(kTW * kTH, glm::vec3(0.0f));
    const float H = std::sqrt(m_p.topRadius * m_p.topRadius - m_p.bottomRadius * m_p.bottomRadius);
    std::vector<int> rows(kTH);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        const float rho = H * float(y) / float(kTH - 1);
        const float r = std::sqrt(rho * rho + m_p.bottomRadius * m_p.bottomRadius);
        const float dMin = m_p.topRadius - r, dMax = rho + H;
        for (int x = 0; x < kTW; ++x) {
            const float d = dMin + float(x) / float(kTW - 1) * (dMax - dMin);
            const float mu = d == 0.0f ? 1.0f : std::clamp((H * H - rho * rho - d * d) / (2.0f * r * d), -1.0f, 1.0f);
            m_transmittance[y * kTW + x] = transmittanceIntegral(r, mu);
        }
    });
}

glm::vec3 Atmosphere::transmittance(float r, float mu) const {
    r = std::clamp(r, m_p.bottomRadius, m_p.topRadius);
    if (hitsGround(r, mu, m_p.bottomRadius)) return glm::vec3(0.0f);
    const float H = std::sqrt(m_p.topRadius * m_p.topRadius - m_p.bottomRadius * m_p.bottomRadius);
    const float rho = std::sqrt(std::max(r * r - m_p.bottomRadius * m_p.bottomRadius, 0.0f));
    const float d = distToSphere(r, mu, m_p.topRadius);
    const float dMin = m_p.topRadius - r, dMax = rho + H;
    return bilinear(m_transmittance, kTW, kTH, (d - dMin) / std::max(dMax - dMin, 1e-3f), rho / H);
}

// Hillaire's multiple scattering: from each point (altitude, sun zenith), 64 directions over the
// sphere, each marched with single scattering of a unit sun by an isotropic phase (L2) and the
// fraction f_ms the medium would scatter back of an isotropic unit of light; every order past the
// first sums to L2 / (1 - f_ms).
void Atmosphere::buildMultipleScattering() {
    m_multiple.assign(kMS * kMS, glm::vec3(0.0f));
    constexpr int kDirs = 8;   // 8 x 8 stratified over the sphere
    constexpr int kSteps = 20;
    std::vector<int> rows(kMS);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        const float r = m_p.bottomRadius + (float(y) + 0.5f) / float(kMS) * (m_p.topRadius - m_p.bottomRadius);
        for (int x = 0; x < kMS; ++x) {
            const float muS = -1.0f + 2.0f * (float(x) + 0.5f) / float(kMS);
            const glm::vec3 sun(std::sqrt(std::max(1.0f - muS * muS, 0.0f)), muS, 0.0f);
            glm::vec3 L2(0.0f), fms(0.0f);
            for (int a = 0; a < kDirs; ++a)
                for (int b = 0; b < kDirs; ++b) {
                    const float cosT = 1.0f - 2.0f * (float(a) + 0.5f) / float(kDirs);
                    const float sinT = std::sqrt(std::max(1.0f - cosT * cosT, 0.0f));
                    const float phi = 2.0f * kPi * (float(b) + 0.5f) / float(kDirs);
                    const glm::vec3 dir(sinT * std::cos(phi), cosT, sinT * std::sin(phi));
                    const float mu = dir.y;
                    const bool ground = hitsGround(r, mu, m_p.bottomRadius);
                    const float tMax = ground ? distToGround(r, mu, m_p.bottomRadius) : distToSphere(r, mu, m_p.topRadius);
                    const float dt = tMax / float(kSteps);
                    glm::vec3 T(1.0f), L(0.0f), f(0.0f);
                    const glm::vec3 origin(0.0f, r, 0.0f);
                    for (int i = 0; i < kSteps; ++i) {
                        const glm::vec3 p = origin + dir * ((float(i) + 0.5f) * dt);
                        const float ri = glm::length(p);
                        const Medium m = mediumAt(ri - m_p.bottomRadius);
                        const glm::vec3 scattering = m.scatteringRayleigh + glm::vec3(m.scatteringMie);
                        const glm::vec3 stepT = glm::exp(-m.extinction * dt);
                        const glm::vec3 integ = (glm::vec3(1.0f) - stepT) / glm::max(m.extinction, glm::vec3(1e-12f));
                        const glm::vec3 sunT = transmittance(ri, glm::dot(p / ri, sun));
                        L += T * scattering * sunT * kIsotropicPhase * integ;
                        f += T * scattering * integ;
                        T *= stepT;
                    }
                    if (ground) {
                        const glm::vec3 g = origin + dir * tMax;
                        const glm::vec3 up = glm::normalize(g);
                        const float c = std::max(glm::dot(up, sun), 0.0f);
                        L += T * transmittance(m_p.bottomRadius, glm::dot(up, sun)) * c * m_p.groundAlbedo / kPi;
                    }
                    const float w = 4.0f * kPi / float(kDirs * kDirs) * kIsotropicPhase;
                    L2 += L * w;
                    fms += f * w;
                }
            m_multiple[y * kMS + x] = L2 / glm::max(glm::vec3(1.0f) - fms, glm::vec3(1e-3f));
        }
    });
}

glm::vec3 Atmosphere::multipleScattering(float r, float muS) const {
    return bilinear(m_multiple, kMS, kMS, (muS + 1.0f) * 0.5f,
                    (r - m_p.bottomRadius) / (m_p.topRadius - m_p.bottomRadius));
}

glm::vec3 Atmosphere::radiance(glm::vec3 dir, glm::vec3 sunDir, glm::vec3 sunIlluminance) const {
    constexpr int kSteps = 40;
    const float r = m_p.bottomRadius + m_p.cameraAltitude;
    const float mu = dir.y;
    const bool ground = hitsGround(r, mu, m_p.bottomRadius);
    const float tMax = ground ? distToGround(r, mu, m_p.bottomRadius) : distToSphere(r, mu, m_p.topRadius);
    const glm::vec3 origin(0.0f, r, 0.0f);
    const float nu = glm::dot(dir, sunDir);
    const float phR = rayleighPhase(nu), phM = miePhase(nu, m_p.mieG);
    // Steps grow quadratically: the dense air near the camera gets the most of them.
    glm::vec3 T(1.0f), L(0.0f);
    float tPrev = 0.0f;
    for (int i = 0; i < kSteps; ++i) {
        const float s = (float(i) + 1.0f) / float(kSteps);
        const float t = tMax * s * s;
        const float dt = t - tPrev;
        const float tMid = 0.5f * (t + tPrev);
        tPrev = t;
        const glm::vec3 p = origin + dir * tMid;
        const float ri = glm::length(p);
        const glm::vec3 up = p / ri;
        const float muS = glm::dot(up, sunDir);
        const Medium m = mediumAt(ri - m_p.bottomRadius);
        const glm::vec3 sunT = transmittance(ri, muS);
        const glm::vec3 S = (m.scatteringRayleigh * phR + glm::vec3(m.scatteringMie * phM)) * sunT +
                            (m.scatteringRayleigh + glm::vec3(m.scatteringMie)) * multipleScattering(ri, muS);
        const glm::vec3 stepT = glm::exp(-m.extinction * dt);
        L += T * S * (glm::vec3(1.0f) - stepT) / glm::max(m.extinction, glm::vec3(1e-12f));
        T *= stepT;
    }
    if (ground) {
        const glm::vec3 g = origin + dir * tMax;
        const glm::vec3 up = glm::normalize(g);
        const float c = std::max(glm::dot(up, sunDir), 0.0f);
        L += T * transmittance(m_p.bottomRadius, glm::dot(up, sunDir)) * c * m_p.groundAlbedo / kPi;
    }
    return L * sunIlluminance;
}

std::vector<float> Atmosphere::equirect(uint32_t width, uint32_t height, glm::vec3 sunDir,
                                        glm::vec3 sunIlluminance) const {
    std::vector<float> px(std::size_t{width} * height * 4u, 0.0f);
    sunDir = glm::normalize(sunDir);
    std::vector<uint32_t> rows(height);
    std::iota(rows.begin(), rows.end(), 0u);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t y) {
        const float elevation = (0.5f - (float(y) + 0.5f) / float(height)) * kPi;   // row 0 the zenith
        for (uint32_t x = 0; x < width; ++x) {
            const float phi = ((float(x) + 0.5f) / float(width) - 0.5f) * 2.0f * kPi;
            const glm::vec3 dir(std::cos(elevation) * std::cos(phi), std::sin(elevation),
                                std::cos(elevation) * std::sin(phi));
            const glm::vec3 L = radiance(dir, sunDir, sunIlluminance);
            float* o = &px[(std::size_t{y} * width + x) * 4u];
            o[0] = L.r;
            o[1] = L.g;
            o[2] = L.b;
            o[3] = 1.0f;
        }
    });
    return px;
}

}  // namespace ohao::render
