#include "actor_controller.h"
#include "scene_sync.h"

#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/material/bindless_texture_manager.hpp"
#include "renderer/components/material_component.hpp"
#include "physics/components/physics_component.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/world/physics_world.hpp"
#include "physics/collision/shapes/collision_shape.hpp"

#include <godot_cpp/variant/utility_functions.hpp>
#include <unordered_map>

namespace godot {

// ===== Transform =====

void ActorController::setPosition(ohao::Scene* scene, const std::string& name, const glm::vec3& position) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) {
        static std::unordered_map<std::string, int> missCount;
        if (missCount[name]++ < 3) {
            UtilityFunctions::printerr("[OHAO] set_actor_position: actor '", name.c_str(), "' NOT FOUND in scene");
        }
        return;
    }
    auto transform = actor->getTransform();
    if (transform) {
        transform->setPosition(position);
    }
}

void ActorController::setRotation(ohao::Scene* scene, const std::string& name, const glm::vec3& rotationDeg) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto transform = actor->getTransform();
    if (transform) {
        glm::vec3 rad(glm::radians(rotationDeg.x), glm::radians(rotationDeg.y), glm::radians(rotationDeg.z));
        transform->setRotation(glm::quat(rad));
    }
}

void ActorController::setScale(ohao::Scene* scene, const std::string& name, const glm::vec3& scale) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto transform = actor->getTransform();
    if (transform) {
        transform->setScale(scale);
    }
}

// ===== Lifecycle =====

void ActorController::removeActor(ohao::Scene* scene, ohao::OffscreenRenderer* renderer, const std::string& name) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (actor) {
        scene->removeActor(actor);
        if (renderer) renderer->updateSceneBuffers();
    }
}

bool ActorController::hasActor(ohao::Scene* scene, const std::string& name) const {
    if (!scene) return false;
    return scene->findActor(name) != nullptr;
}

// ===== Texture / Material =====

void ActorController::setTexture(ohao::Scene* scene, ohao::OffscreenRenderer* renderer,
                                  const std::string& actorName, const std::string& texturePath) {
    if (!scene || !renderer) return;

    auto* texMgr = renderer->getTextureManager();
    if (!texMgr) {
        UtilityFunctions::printerr("[OHAO] Texture manager not available");
        return;
    }

    auto actor = scene->findActor(actorName);
    if (!actor) {
        UtilityFunctions::printerr("[OHAO] Actor not found: ", actorName.c_str());
        return;
    }

    auto handle = texMgr->loadTexture(texturePath, ohao::BindlessTextureType::Albedo);
    if (!handle.valid()) {
        UtilityFunctions::printerr("[OHAO] Failed to load texture: ", texturePath.c_str());
        return;
    }
    texMgr->updateDescriptorSet();

    auto materialComp = actor->getComponent<ohao::MaterialComponent>();
    if (materialComp) {
        materialComp->setAlbedoTexture(texturePath);
    }

    UtilityFunctions::print("[OHAO] Texture set on '", actorName.c_str(), "': ", texturePath.c_str());
}

void ActorController::setNormalMap(ohao::Scene* scene, ohao::OffscreenRenderer* renderer,
                                    const std::string& actorName, const std::string& normalPath) {
    if (!scene || !renderer) return;

    auto* texMgr = renderer->getTextureManager();
    if (!texMgr) return;

    auto actor = scene->findActor(actorName);
    if (!actor) {
        UtilityFunctions::printerr("[OHAO] Actor not found: ", actorName.c_str());
        return;
    }

    auto handle = texMgr->loadTexture(normalPath, ohao::BindlessTextureType::Normal);
    if (!handle.valid()) {
        UtilityFunctions::printerr("[OHAO] Failed to load normal map: ", normalPath.c_str());
        return;
    }
    texMgr->updateDescriptorSet();

    auto materialComp = actor->getComponent<ohao::MaterialComponent>();
    if (materialComp) {
        materialComp->setNormalTexture(normalPath);
    }

    UtilityFunctions::print("[OHAO] Normal map set on '", actorName.c_str(), "': ", normalPath.c_str());
}

void ActorController::setPBR(ohao::Scene* scene, const std::string& actorName, float metallic, float roughness) {
    if (!scene) return;

    auto actor = scene->findActor(actorName);
    if (!actor) {
        UtilityFunctions::printerr("[OHAO] Actor not found: ", actorName.c_str());
        return;
    }

    auto materialComp = actor->getComponent<ohao::MaterialComponent>();
    if (materialComp) {
        materialComp->getMaterial().metallic = metallic;
        materialComp->getMaterial().roughness = roughness;
    }
}

// ===== Physics Properties =====

int ActorController::getBodyHandle(ohao::Scene* scene, const std::string& name) {
    if (!scene) return -1;
    auto actor = scene->findActor(name);
    if (!actor) return -1;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (!phys || !phys->getRigidBody()) return -1;
    auto handle = phys->getRigidBody()->getBackendHandle();
    return (handle == ohao::physics::backend::INVALID_BODY) ? -1 : static_cast<int>(handle);
}

void ActorController::setBodyType(ohao::Scene* scene, const std::string& name, int type) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (!phys) return;
    ohao::physics::dynamics::RigidBodyType rt;
    switch (type) {
        case 1: rt = ohao::physics::dynamics::RigidBodyType::STATIC; break;
        case 2: rt = ohao::physics::dynamics::RigidBodyType::KINEMATIC; break;
        default: rt = ohao::physics::dynamics::RigidBodyType::DYNAMIC; break;
    }
    phys->setRigidBodyType(rt);
}

void ActorController::setMass(ohao::Scene* scene, const std::string& name, float mass) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setMass(mass);
}

void ActorController::setRestitution(ohao::Scene* scene, const std::string& name, float restitution) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setRestitution(restitution);
}

void ActorController::setFriction(ohao::Scene* scene, const std::string& name, float friction) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setFriction(friction);
}

void ActorController::setGravityEnabled(ohao::Scene* scene, const std::string& name, bool enabled) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setGravityEnabled(enabled);
}

void ActorController::setGravityScale(ohao::Scene* scene, const std::string& name, float scale) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setGravityScale(scale);
}

void ActorController::applyRadialImpulse(ohao::Scene* scene, const glm::vec3& center, float strength, float radius, int falloff) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld && physWorld->hasBackend()) {
        physWorld->applyRadialImpulse(center, strength, radius, falloff);
    }
}

void ActorController::setLinearVelocity(ohao::Scene* scene, const std::string& name, const glm::vec3& velocity) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    if (phys) phys->setLinearVelocity(velocity);
}

void ActorController::syncPhysicsShape(ohao::Scene* scene, const std::string& name) {
    if (!scene) return;
    auto actor = scene->findActor(name);
    if (!actor) return;
    auto phys = actor->getComponent<ohao::PhysicsComponent>();
    auto transform = actor->getTransform();
    if (!phys || !transform) return;

    glm::vec3 scale = transform->getScale();

    auto existingShape = phys->getCollisionShape();
    if (existingShape) {
        using ST = ohao::physics::collision::ShapeType;
        switch (existingShape->getType()) {
            case ST::SPHERE: {
                float radius = (scale.x + scale.y + scale.z) / 6.0f;
                phys->createSphereShape(radius);
                return;
            }
            case ST::CAPSULE: {
                float radius = scale.x * 0.5f;
                float height = scale.y;
                phys->createCapsuleShape(radius, height);
                return;
            }
            case ST::CYLINDER: {
                float radius = scale.x * 0.5f;
                float height = scale.y;
                phys->createCylinderShape(radius, height);
                return;
            }
            default:
                break;
        }
    }
    phys->createBoxShape(glm::vec3(scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f));
}

} // namespace godot
