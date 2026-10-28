#pragma once
// A tracer's quad (sub-project 12, step 3): the closed form of where its streak is at an age, as the
// renderer draws it (render/vfx: six vertices a tracer, additive light). Plain math, no Vulkan, so the
// tests check the timing and the facing without a device.
//
// The round runs from `from` (the muzzle) to `to` (its hit, or 60 m on a miss). The streak's head is
// at speed * age along the round, clamped at `to`; its tail is `length` behind the head, clamped at
// the head (so the streak shortens onto the hit and vanishes). Its width is across the round and
// faces the eye.

#include <glm/glm.hpp>

#include <array>
#include <cstddef>

namespace ohao::vfx {

struct TracerQuad {
    struct Corner {
        glm::vec3 pos{0.0f};
        float across{0.0f};   // -1 to 1, across the streak
        float along{0.0f};    // 0 at the tail, 1 at the head
    };
    std::array<Corner, 6> corners{};   // two triangles, counter-clockwise seen from the eye
    glm::vec3 centre{0.0f};            // the middle of the streak (the sort's distance)
    float head{0.0f};                  // m along the round from `from`
    float tail{0.0f};
    bool visible{false};               // false before the round has been fired (head at 0)
};

inline TracerQuad tracerQuad(glm::vec3 from, glm::vec3 to, float speed, float length, float width, float age,
                             glm::vec3 eye) {
    TracerQuad q;
    const glm::vec3 span = to - from;
    const float len = glm::length(span);
    if (len < 1e-5f || speed <= 0.0f || age < 0.0f) return q;
    const glm::vec3 dir = span / len;
    q.head = glm::min(speed * age, len);
    q.tail = glm::max(0.0f, q.head - length);
    if (q.head <= q.tail) return q;
    q.visible = true;
    const glm::vec3 p0 = from + dir * q.tail, p1 = from + dir * q.head;
    q.centre = from + dir * (0.5f * (q.head + q.tail));
    glm::vec3 side = glm::cross(dir, eye - q.centre);
    const float sideLength = glm::length(side);
    side = sideLength > 1e-6f ? side * (0.5f * width / sideLength) : glm::vec3(0.0f);
    const glm::vec3 a0 = p0 - side, b0 = p0 + side, a1 = p1 - side, b1 = p1 + side;
    q.corners = {{
        {a0, -1.0f, 0.0f},
        {b0, 1.0f, 0.0f},
        {b1, 1.0f, 1.0f},
        {a0, -1.0f, 0.0f},
        {b1, 1.0f, 1.0f},
        {a1, -1.0f, 1.0f},
    }};
    return q;
}

} // namespace ohao::vfx
