#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>

// Forward declare OHAO types
namespace ohao {
    class Scene;
}

namespace ohao::physics::backend {
    struct RaycastHit;
    struct ConstraintSettings;
    struct CharacterCreationInfo;
    struct CharacterState;
    enum class ConstraintType;
    enum class GroundState;
}

namespace godot {

/**
 * PhysicsController - Physics simulation, raycasting, constraints, character controller
 *
 * Extracted from OhaoViewport. Plain C++ class (not a Godot node), owned by OhaoViewport.
 * Receives ohao::Scene* as a parameter — does not store pointers.
 */
class PhysicsController {
public:
    PhysicsController() = default;

    // === Physics Simulation ===
    bool isPlaying() const { return m_playing; }
    float getSpeed() const { return m_speed; }
    void setSpeed(float speed) { m_speed = speed; }

    void play(ohao::Scene* scene);
    void pause(ohao::Scene* scene);
    void step(ohao::Scene* scene);
    void stop(ohao::Scene* scene);

    // === Raycasting ===
    struct RayHitResult {
        bool hit = false;
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        float fraction = 0.0f;
        uint32_t bodyHandle = 0;
        uint16_t layer = 0;
    };

    RayHitResult castRay(ohao::Scene* scene, const glm::vec3& origin, const glm::vec3& direction,
                         float maxDistance, uint16_t layerMask);

    struct RayHitEntry {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        float fraction = 0.0f;
        uint32_t bodyHandle = 0;
        uint16_t layer = 0;
    };

    std::vector<RayHitEntry> castRayAll(ohao::Scene* scene, const glm::vec3& origin, const glm::vec3& direction,
                                         float maxDistance, uint16_t layerMask);

    std::vector<uint32_t> overlapSphere(ohao::Scene* scene, const glm::vec3& center, float radius, uint16_t layerMask);
    std::vector<uint32_t> overlapBox(ohao::Scene* scene, const glm::vec3& center, const glm::vec3& halfExtents,
                                      const glm::quat& rotation, uint16_t layerMask);

    // === Collision Layers ===
    void setLayerCollision(ohao::Scene* scene, uint16_t layer1, uint16_t layer2, bool shouldCollide);

    // === Constraints ===
    int createConstraintFixed(ohao::Scene* scene, uint32_t body1, uint32_t body2, const glm::vec3& anchor);
    int createConstraintHinge(ohao::Scene* scene, uint32_t body1, uint32_t body2, const glm::vec3& anchor,
                              const glm::vec3& axis, float limitMin, float limitMax);
    int createConstraintSlider(ohao::Scene* scene, uint32_t body1, uint32_t body2, const glm::vec3& axis,
                               float limitMin, float limitMax);
    int createConstraintPoint(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                              const glm::vec3& anchor1, const glm::vec3& anchor2);
    int createConstraintDistance(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                const glm::vec3& anchor1, const glm::vec3& anchor2,
                                float minDist, float maxDist);
    int createConstraintCone(ohao::Scene* scene, uint32_t body1, uint32_t body2, const glm::vec3& anchor,
                             const glm::vec3& twistAxis, float halfConeAngle);
    void destroyConstraint(ohao::Scene* scene, uint32_t handle);
    void setConstraintEnabled(ohao::Scene* scene, uint32_t handle, bool enabled);
    void setConstraintMotor(ohao::Scene* scene, uint32_t handle, bool enabled, float speed, float maxForce);
    void setConstraintLimits(ohao::Scene* scene, uint32_t handle, float minVal, float maxVal);
    void setConstraintBreaking(ohao::Scene* scene, uint32_t handle, float maxForce, float maxTorque);
    std::vector<uint32_t> getAndClearBrokenConstraints(ohao::Scene* scene);

    // === Character Controller ===
    int createCharacter(ohao::Scene* scene, const glm::vec3& position, float capsuleRadius, float capsuleHeight,
                        float maxSlopeDeg, float mass);
    void destroyCharacter(ohao::Scene* scene, uint32_t handle);

    struct CharacterStateResult {
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 groundNormal{0.0f};
        int groundState = 3; // IN_AIR
        int groundBody = -1;
    };

    CharacterStateResult getCharacterState(ohao::Scene* scene, uint32_t handle);
    void setCharacterPosition(ohao::Scene* scene, uint32_t handle, const glm::vec3& position);
    void setCharacterRotation(ohao::Scene* scene, uint32_t handle, const glm::quat& rotation);
    void setCharacterVelocity(ohao::Scene* scene, uint32_t handle, const glm::vec3& velocity);
    void updateCharacter(ohao::Scene* scene, uint32_t handle, float delta,
                         const glm::vec3& gravity, const glm::vec3& movementInput);

    // === Physics Grab/Throw ===
    // Grab a dynamic body with a mouse-spring (POINT constraint to a kinematic ghost body).
    // Returns a grab token (>= 0), or -1 on failure.
    int grabBody(ohao::Scene* scene, uint32_t bodyHandle, const glm::vec3& worldPos);
    void moveGrab(ohao::Scene* scene, int token, const glm::vec3& worldPos);
    void releaseGrab(ohao::Scene* scene, int token);
    void throwGrab(ohao::Scene* scene, int token, const glm::vec3& velocity);

private:
    bool m_playing = false;
    float m_speed = 1.0f;

    struct GrabState {
        uint32_t grabbedBody   = 0; // the dynamic body being dragged
        uint32_t ghostBody     = 0; // kinematic anchor body
        uint32_t constraint    = 0; // POINT constraint linking them
    };
    std::unordered_map<int, GrabState> m_grabs;
    int m_nextGrabToken = 0;
};

} // namespace godot
