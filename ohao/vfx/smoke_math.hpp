#pragma once
// Smoke's proxy spheres (sub-project 12, step 4; docs/specs/2026-10-28-vfx-design.md, "Smoke in RT GI
// and shadows", OPEN ruling 18): the closed form of a smoke record's density spheres at its age. The
// RT shadow and GI traces attenuate through them (shaders/rt/smoke_proxy.slang), the renderer uploads
// them each frame (render/rt/rt_alpha_tables), the CPU tests read the same numbers. Nothing here is
// integrated: a sphere at age t is the same in every host, after any history of frames.
//
// Sphere i of a record (u(k) = vfxUnit-style hash of (seed, i, k)):
//   final radius R_i   = lerp(radius.min, radius.max, u(0))
//   offset             = (cos a, 0, sin a) * 0.6 R_i,  a = 2 pi u(1), lifted by 0.3 R_i
//   centre(age)        = pose * (offset + (0, rise * age, 0))
//   radius(age)        = R_i * (0.5 + 0.5 t)                       t = age / life
//   density(age)      = density * min(age / 0.25 s, 1) * (1 - t)   (zero from `life` on)
// A record past its life has no spheres.
#include "vfx/particle_math.hpp"
#include "vfx/vfx_set.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ohao::vfx {

struct SmokeSphere {
    glm::vec3 centre{0.0f};   // world
    float radius{0.0f};      // m
    float density{0.0f};     // 1/m: transmittance exp(-density * chord)
};

// The spheres of one smoke record (`proxy` at `pose`, seeded by `seed`) at `age` seconds, appended to
// `out` in sphere order. Nothing for a negative age or one past the proxy's life.
inline void smokeSpheres(const ProxyDesc& proxy, uint32_t seed, const glm::mat4& pose, float age,
                         std::vector<SmokeSphere>& out) {
    if (age < 0.0f || proxy.life <= 0.0f || age >= proxy.life) return;
    const float t = age / proxy.life;
    const float fadeIn = std::min(age / 0.25f, 1.0f);
    const float density = proxy.density * fadeIn * (1.0f - t);
    const auto unit = [seed](uint32_t i, uint32_t k) {
        return static_cast<float>(vfxHash(seed, i, 0x200u + k) >> 8) * (1.0f / 16777216.0f);
    };
    for (uint32_t i = 0; i < proxy.spheres; ++i) {
        const float finalRadius = proxy.radius.min + (proxy.radius.max - proxy.radius.min) * unit(i, 0);
        const float a = glm::two_pi<float>() * unit(i, 1);
        const glm::vec3 offset(std::cos(a) * 0.6f * finalRadius, 0.3f * finalRadius, std::sin(a) * 0.6f * finalRadius);
        SmokeSphere s;
        s.centre = glm::vec3(pose * glm::vec4(offset + glm::vec3(0.0f, proxy.rise * age, 0.0f), 1.0f));
        s.radius = finalRadius * (0.5f + 0.5f * t);
        s.density = density;
        out.push_back(s);
    }
}

} // namespace ohao::vfx
