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
    float roomHalfSize;       // room S value
    float modelScale;         // uniform scale applied to model
    glm::vec3 modelPosition;  // translation to center model in room
    glm::quat modelRotation;  // rotation to face camera (Y-up correction)
    glm::vec3 cameraPosition; // camera world position
    float cameraFov;          // camera FOV in degrees

    struct LightSetup {
        std::string name;
        glm::vec3 position;
        glm::vec3 color;
        float intensity;
        float radius;
    };
    std::vector<LightSetup> lights;
};

class SceneFramer {
public:
    // Compute framing for a model based on its vertex data
    // forceYUp: set true for formats that guarantee Y-up (GLB/glTF)
    static FrameResult computeFraming(const std::vector<Vertex>& vertices, bool forceYUp = false);

    // Apply lights from a FrameResult to a scene
    static void applyLights(Scene* scene, const FrameResult& result);

    // Apply camera settings from a FrameResult
    static void applyCamera(Camera& camera, const FrameResult& result);
};

} // namespace ohao
