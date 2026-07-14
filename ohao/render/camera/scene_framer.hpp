#pragma once
// SceneFramer — auto-frame any model with correct camera, lights, and room size.
// Analyzes model AABB, scales to fit, and positions camera + lights automatically.
// Works for ANY model size (mm, cm, m, arbitrary units).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cfloat>
#include <vector>
#include <string>

namespace ohao {

class Scene;
class Camera;
struct Vertex;

struct FrameResult {
    float roomHalfSize{0.f};
    float modelScale{1.f};
    glm::vec3 modelPosition{0.f};
    glm::quat modelRotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 cameraPosition{0.f};
    float cameraFov{45.f};

    struct LightSetup {
        std::string name;
        glm::vec3 position{0.f};
        glm::vec3 color{1.f};
        float intensity{1.f};
        float radius{0.5f};
    };
    std::vector<LightSetup> lights;

    [[nodiscard]] bool valid() const noexcept {
        return roomHalfSize > 0.f && modelScale > 0.f && cameraFov > 0.f;
    }
};

class SceneFramer {
public:
    // Compute framing for a model based on its vertex data
    // forceYUp: set true for formats that guarantee Y-up (GLB/glTF)
    [[nodiscard]] static FrameResult computeFraming(const std::vector<Vertex>& vertices,
                                                    bool forceYUp = false);

    // Apply lights from a FrameResult to a scene
    static void applyLights(Scene* scene, const FrameResult& result);

    // Apply camera settings from a FrameResult
    static void applyCamera(Camera& camera, const FrameResult& result);
};

} // namespace ohao
