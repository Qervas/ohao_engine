#pragma once

// Simple pinhole for Diff-IR ground-plane projection (matches studio yaw/pitch convention).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace ohao::diff {

struct DiffCamera {
    glm::vec3 position{0.f, 1.6f, 4.5f};
    float pitchDeg{0.f};
    float yawDeg{0.f};
    float vfovDeg{45.f};
    float aspect{16.f / 9.f};
    float nearZ{0.05f};
    float farZ{200.f};

    [[nodiscard]] glm::mat4 viewMatrix() const {
        const float pitch = glm::radians(pitchDeg);
        const float yaw = glm::radians(yawDeg);
        const glm::vec3 forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                -std::cos(yaw) * std::cos(pitch)};
        const glm::vec3 target = position + glm::normalize(forward);
        return glm::lookAt(position, target, glm::vec3(0.f, 1.f, 0.f));
    }

    [[nodiscard]] glm::mat4 projMatrix() const {
        glm::mat4 p = glm::perspective(glm::radians(vfovDeg), aspect, nearZ, farZ);
        p[1][1] *= -1.f; // Vulkan-style Y flip for NDC
        return p;
    }

    /// Unproject pixel center to world ray; intersect y=0 plane. Returns false if miss.
    [[nodiscard]] bool groundHit(float px, float py, std::uint32_t w, std::uint32_t h, float& u,
                                 float& v, float& worldX, float& worldZ) const {
        if (w == 0 || h == 0) return false;
        const float ndcX = (2.f * (px + 0.5f) / static_cast<float>(w)) - 1.f;
        const float ndcY = (2.f * (py + 0.5f) / static_cast<float>(h)) - 1.f;
        const glm::mat4 invVP = glm::inverse(projMatrix() * viewMatrix());
        glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, 0.f, 1.f);
        glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.f, 1.f);
        nearH /= nearH.w;
        farH /= farH.w;
        const glm::vec3 origin = glm::vec3(nearH);
        const glm::vec3 dir = glm::normalize(glm::vec3(farH) - origin);
        if (std::abs(dir.y) < 1e-5f) return false;
        const float t = -origin.y / dir.y;
        if (t < 0.f) return false;
        const glm::vec3 hit = origin + t * dir;
        constexpr float half = 14.f;
        if (hit.x < -half || hit.x > half || hit.z < -half || hit.z > half) return false;
        worldX = hit.x;
        worldZ = hit.z;
        u = (hit.x + half) / (2.f * half);
        v = (hit.z + half) / (2.f * half);
        return true;
    }
};

} // namespace ohao::diff
