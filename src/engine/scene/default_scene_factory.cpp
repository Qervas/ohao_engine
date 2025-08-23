#include "default_scene_factory.hpp"
#include "ui/components/console_widget.hpp"

namespace ohao {

std::unique_ptr<Scene> DefaultSceneFactory::createBlenderLikeScene() {
    auto scene = std::make_unique<Scene>("Default Scene");
    
    OHAO_LOG("Creating Blender-like default scene...");
    
    // 1. Default Directional Light (like Blender's sun lamp)
    auto lightActor = scene->createActorWithComponents("Sun Light", PrimitiveType::DirectionalLight);
    if (lightActor) {
        auto transform = lightActor->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(4.0f, 8.0f, 7.0f));
        }
    }
    
    // 2. Default Sphere (like Blender's default cube, but we use sphere)
    auto sphereActor = scene->createActorWithComponents("Sphere", PrimitiveType::Sphere);
    if (sphereActor) {
        auto transform = sphereActor->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(0.0f, 2.0f, 0.0f)); // Start above ground
            
            // CRITICAL FIX: Sync physics body with updated transform position
            auto physicsComponent = sphereActor->getComponent<PhysicsComponent>();
            if (physicsComponent) {
                physicsComponent->updateRigidBodyFromTransform();
            }
        }
    }
    
    // 3. Ground Plane for physics interaction
    auto groundActor = scene->createActorWithComponents("Ground Platform", PrimitiveType::Platform);
    if (groundActor) {
        auto transform = groundActor->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(0.0f, -0.1f, 0.0f));
            transform->setScale(glm::vec3(10.0f, 1.0f, 10.0f)); // Large ground
            
            // CRITICAL FIX: Sync physics body with updated transform position  
            auto physicsComponent = groundActor->getComponent<PhysicsComponent>();
            if (physicsComponent) {
                physicsComponent->updateRigidBodyFromTransform();
            }
        }
    }
    
    OHAO_LOG("Default scene created successfully with " + 
             std::to_string(scene->getAllActors().size()) + " actors");
    
    return scene;
}

std::unique_ptr<Scene> DefaultSceneFactory::createEmptyScene() {
    auto scene = std::make_unique<Scene>("Empty Scene");
    
    // Just add basic directional lighting
    auto lightActor = scene->createActorWithComponents("Directional Light", PrimitiveType::DirectionalLight);
    if (lightActor) {
        auto transform = lightActor->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(2.0f, 4.0f, 3.0f));
        }
    }
    
    OHAO_LOG("Empty scene created with basic lighting");
    return scene;
}

std::unique_ptr<Scene> DefaultSceneFactory::createPhysicsTestScene() {
    auto scene = std::make_unique<Scene>("Physics Test Scene");
    
    OHAO_LOG("Creating physics test scene...");
    
    // Ground plane
    auto ground = scene->createActorWithComponents("Ground", PrimitiveType::Platform);
    if (ground) {
        auto transform = ground->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(0.0f, -0.1f, 0.0f));
            transform->setScale(glm::vec3(15.0f, 1.0f, 15.0f));
            
            // CRITICAL FIX: Sync physics body with updated transform position
            auto physicsComponent = ground->getComponent<PhysicsComponent>();
            if (physicsComponent) {
                physicsComponent->updateRigidBodyFromTransform();
            }
        }
    }
    
    // Multiple test objects at different positions
    std::vector<std::pair<std::string, PrimitiveType>> testObjects = {
        {"Test Sphere 1", PrimitiveType::Sphere},
        {"Test Cube 1", PrimitiveType::Cube},
        {"Test Sphere 2", PrimitiveType::Sphere},
        {"Test Cube 2", PrimitiveType::Cube}
    };
    
    float xOffset = -3.0f;
    for (const auto& [name, type] : testObjects) {
        auto actor = scene->createActorWithComponents(name, type);
        if (actor) {
            auto transform = actor->getTransform();
            if (transform) {
                transform->setPosition(glm::vec3(xOffset, 3.0f, 0.0f));
                
                // CRITICAL FIX: Sync physics body with updated transform position
                auto physicsComponent = actor->getComponent<PhysicsComponent>();
                if (physicsComponent) {
                    physicsComponent->updateRigidBodyFromTransform();
                }
                
                xOffset += 2.0f;
            }
        }
    }
    
    // Directional light
    auto light = scene->createActorWithComponents("Sun Light", PrimitiveType::DirectionalLight);
    if (light) {
        auto transform = light->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(5.0f, 10.0f, 8.0f));
        }
    }
    
    OHAO_LOG("Physics test scene created with " + 
             std::to_string(scene->getAllActors().size()) + " actors");
    
    return scene;
}

void DefaultSceneFactory::setupDefaultLighting(Scene* scene) {
    // This is handled by createActorWithComponents now
    // Left for future expansion if needed
}

void DefaultSceneFactory::setupGroundPlane(Scene* scene) {
    // This is handled by createActorWithComponents now
    // Left for future expansion if needed
}

void DefaultSceneFactory::setupDefaultCamera(Scene* scene) {
    // Future: When we move to entity-based camera system
    // For now, camera is handled by VulkanContext
}

} // namespace ohao