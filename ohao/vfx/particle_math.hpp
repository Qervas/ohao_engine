#pragma once
// A particle as a closed form of its record and its age (sub-project 12, docs/specs/
// 2026-10-28-vfx-design.md, "Determinism"): nothing is integrated, so a particle at age t is the
// same in every host, after any history of frames. The renderer evaluates the same expressions on
// the GPU (Task 2); this is the CPU reference the tests and vfx.info use (positions agree within
// 1e-4 m; they are never compared bit for bit).
//
// Randomness is counter-based: vfxHash(seed, particle index, attribute), no generator state.
//
// Position under gravity g (m/s^2 along -Y) and drag k (1/s), from x0 with velocity v0:
//   x(t) = x0 + v0 (1 - e^-kt) / k + G (t/k - (1 - e^-kt) / k^2),  G = (0, -g, 0)
//   x(t) = x0 + v0 t + G t^2 / 2                                   (k = 0)
#include "vfx/vfx_set.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ohao::vfx {

// Attributes drawn per particle (the third argument of vfxHash).
enum class Draw : uint32_t { Life = 1, Speed = 2, ConeZ = 3, ConePhi = 4, Frame = 5 };

// A 32-bit hash of three words (a PCG step folded with the inputs): deterministic and portable.
[[nodiscard]] inline uint32_t vfxHash(uint32_t seed, uint32_t index, uint32_t attribute) noexcept {
    uint32_t h = seed * 0x9E3779B1u ^ (index + 0x7F4A7C15u) * 0x85EBCA77u ^ (attribute + 0x165667B1u) * 0xC2B2AE3Du;
    h = h * 747796405u + 2891336453u;
    const uint32_t word = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    return (word >> 22u) ^ word;
}

// In [0, 1), 24 bits.
[[nodiscard]] inline float vfxUnit(uint32_t seed, uint32_t index, Draw attribute) noexcept {
    return static_cast<float>(vfxHash(seed, index, static_cast<uint32_t>(attribute)) >> 8) * (1.0f / 16777216.0f);
}

template <class T>
[[nodiscard]] T evaluateCurve(const Curve<T>& c, float t) noexcept {
    if (c.keys.empty()) return T(1);
    if (t <= c.keys.front().first) return c.keys.front().second;
    for (std::size_t i = 1; i < c.keys.size(); ++i)
        if (t <= c.keys[i].first) {
            const auto& [t0, v0] = c.keys[i - 1];
            const auto& [t1, v1] = c.keys[i];
            const float span = t1 - t0;
            const float f = span > 0.0f ? (t - t0) / span : 1.0f;
            return v0 + (v1 - v0) * f;
        }
    return c.keys.back().second;
}

// Position after `t` seconds from x0 at v0 under gravity `g` and drag `k` (the closed form above).
[[nodiscard]] inline glm::vec3 ballistic(glm::vec3 x0, glm::vec3 v0, float g, float k, float t) noexcept {
    const glm::vec3 G(0.0f, -g, 0.0f);
    if (k < 1e-6f) return x0 + v0 * t + G * (0.5f * t * t);
    const float decay = 1.0f - std::exp(-k * t);
    return x0 + v0 * (decay / k) + G * (t / k - decay / (k * k));
}

// A direction within `spreadDeg` of `axis` (unit), uniform over the cone's solid angle.
[[nodiscard]] inline glm::vec3 coneDirection(glm::vec3 axis, float spreadDeg, float u, float v) noexcept {
    const float cosMax = std::cos(glm::radians(std::clamp(spreadDeg, 0.0f, 180.0f)));
    const float z = 1.0f - u * (1.0f - cosMax);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = 6.2831853f * v;
    const glm::vec3 a = std::fabs(axis.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 x = glm::normalize(glm::cross(a, axis));
    const glm::vec3 y = glm::cross(axis, x);
    return glm::normalize(x * (r * std::cos(phi)) + y * (r * std::sin(phi)) + axis * z);
}

struct ParticleState {
    bool alive{false};
    float age{0.0f}, life{0.0f};     // s
    glm::vec3 position{0.0f};
    glm::vec3 velocity0{0.0f};       // at spawn
    float size{0.0f};
    glm::vec3 color{0.0f};
    float alpha{0.0f};
    uint32_t frame{0};
};

// Particle `index` of an emitter whose record has `seed` and `pose` (origin at its translation,
// thrown along its -Z), `age` seconds after the record's spawn. `frames`: the texture's frame count.
[[nodiscard]] inline ParticleState evaluate(const EmitterDesc& em, uint32_t seed, uint32_t index, float age,
                                            const glm::mat4& pose, uint32_t frames = 1) noexcept {
    ParticleState p;
    p.age = age;
    p.life = em.life.min + (em.life.max - em.life.min) * vfxUnit(seed, index, Draw::Life);
    p.alive = age >= 0.0f && age < p.life;
    const glm::vec3 axis = glm::normalize(-glm::vec3(pose[2]));
    const glm::vec3 dir =
        coneDirection(axis, em.spreadDeg, vfxUnit(seed, index, Draw::ConeZ), vfxUnit(seed, index, Draw::ConePhi));
    const float speed = em.speed.min + (em.speed.max - em.speed.min) * vfxUnit(seed, index, Draw::Speed);
    const glm::vec3 x0 = glm::vec3(pose[3]) + axis * em.offset;
    p.velocity0 = dir * speed;
    const float t = std::max(age, 0.0f);
    p.position = ballistic(x0, p.velocity0, em.gravity, em.drag, t);
    const float f = p.life > 0.0f ? std::clamp(t / p.life, 0.0f, 1.0f) : 1.0f;
    p.size = em.sizeStart + (em.sizeEnd - em.sizeStart) * f;
    p.color = evaluateCurve(em.color, f) * em.intensity;
    p.alpha = evaluateCurve(em.alpha, f);
    const uint32_t n = std::max<uint32_t>(frames, 1);
    switch (em.frames) {
    case FrameMode::Life: p.frame = std::min(n - 1, static_cast<uint32_t>(f * static_cast<float>(n))); break;
    case FrameMode::Random: p.frame = vfxHash(seed, index, static_cast<uint32_t>(Draw::Frame)) % n; break;
    case FrameMode::Fixed: p.frame = std::min(em.frame, n - 1); break;
    }
    return p;
}

} // namespace ohao::vfx
