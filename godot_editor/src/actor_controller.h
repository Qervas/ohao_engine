#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

// Forward declare OHAO types
namespace ohao {
    class Scene;
    class OffscreenRenderer;
}

namespace godot {

// Forward declare for resolveResPath
class SceneSync;

/**
 * ActorController - Actor transforms, lifecycle, physics properties, materials
 *
 * Extracted from OhaoViewport. Plain C++ class (not a Godot node), owned by OhaoViewport.
 * Receives ohao::Scene* / ohao::OffscreenRenderer* as parameters — does not store pointers.
 */
class ActorController {
public:
    ActorController() = default;

    // === Transform ===
    void setPosition(ohao::Scene* scene, const std::string& name, const glm::vec3& position);
    void setRotation(ohao::Scene* scene, const std::string& name, const glm::vec3& rotationDeg);
    void setScale(ohao::Scene* scene, const std::string& name, const glm::vec3& scale);

    // === Lifecycle ===
    void removeActor(ohao::Scene* scene, ohao::OffscreenRenderer* renderer, const std::string& name);
    bool hasActor(ohao::Scene* scene, const std::string& name) const;

    // === Texture / Material ===
    void setTexture(ohao::Scene* scene, ohao::OffscreenRenderer* renderer,
                    const std::string& actorName, const std::string& texturePath);
    void setNormalMap(ohao::Scene* scene, ohao::OffscreenRenderer* renderer,
                     const std::string& actorName, const std::string& normalPath);
    void setPBR(ohao::Scene* scene, const std::string& actorName, float metallic, float roughness);

    // === Physics Properties ===
    int getBodyHandle(ohao::Scene* scene, const std::string& name);
    void setBodyType(ohao::Scene* scene, const std::string& name, int type);
    void setMass(ohao::Scene* scene, const std::string& name, float mass);
    void setRestitution(ohao::Scene* scene, const std::string& name, float restitution);
    void setFriction(ohao::Scene* scene, const std::string& name, float friction);
    void setGravityEnabled(ohao::Scene* scene, const std::string& name, bool enabled);
    void setGravityScale(ohao::Scene* scene, const std::string& name, float scale);
    void setLinearDamping(ohao::Scene* scene, const std::string& name, float damping);
    void setAngularDamping(ohao::Scene* scene, const std::string& name, float damping);
    void applyRadialImpulse(ohao::Scene* scene, const glm::vec3& center, float strength, float radius, int falloff);
    void setLinearVelocity(ohao::Scene* scene, const std::string& name, const glm::vec3& velocity);
    void syncPhysicsShape(ohao::Scene* scene, const std::string& name);
};

} // namespace godot
