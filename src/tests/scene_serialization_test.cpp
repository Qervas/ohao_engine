#include "../core/scene/scene.hpp"
#include "../core/actor/actor.hpp"
#include "../core/component/mesh_component.hpp"
#include "../core/component/physics_component.hpp"
#include "../core/component/light_component.hpp"
#include "../core/serialization/scene_serializer.hpp"
#include <iostream>
#include <memory>

using namespace ohao;

void testSceneSerialization() {
    std::cout << "Testing Scene Serialization/Deserialization" << std::endl;
    
    // Create a test scene
    auto scene = std::make_unique<Scene>("Test Scene");
    
    // Add some actors
    auto cube = scene->createActor("Cube");
    auto meshComp = cube->addComponent<MeshComponent>();
    auto physicsComp = cube->addComponent<PhysicsComponent>();
    physicsComp->createBoxShape(glm::vec3(1.0f, 1.0f, 1.0f));
    
    auto sphere = scene->createActor("Sphere");
    sphere->getTransform()->setPosition(glm::vec3(3.0f, 0.0f, 0.0f));
    auto sphereMesh = sphere->addComponent<MeshComponent>();
    auto spherePhysics = sphere->addComponent<PhysicsComponent>();
    spherePhysics->createSphereShape(1.0f);
    
    // Create a test light using LightComponent
    auto lightActor = scene->createActor("TestLight");
    auto lightComponent = lightActor->addComponent<LightComponent>();
    lightComponent->setLightType(LightType::Point);
    lightComponent->setPosition(glm::vec3(5.0f, 5.0f, 5.0f));
    lightComponent->setColor(glm::vec3(1.0f, 0.9f, 0.8f));
    lightComponent->setIntensity(1.5f);
    
    // Set scene descriptor information
    SceneDescriptor desc;
    desc.name = "Test Scene";
    desc.version = "1.0";
    desc.tags = {"test", "serialization"};
    desc.createdBy = "Scene Serialization Test";
    desc.lastModified = "0";  // Will be updated during save
    scene->setDescriptor(desc);
    
    // Create a serializer
    SceneSerializer serializer(scene.get());
    
    // Save the scene to a file
    std::string scenePath = "test_scenes/test_scene.ohscene";
    if (serializer.serialize(scenePath)) {
        std::cout << "Successfully saved scene to: " << scenePath << std::endl;
    } else {
        std::cerr << "Failed to save scene!" << std::endl;
        return;
    }
    
    // Create a new scene to load into
    auto loadedScene = std::make_unique<Scene>();
    SceneSerializer loadSerializer(loadedScene.get());
    
    // Load the scene from the file
    if (loadSerializer.deserialize(scenePath)) {
        std::cout << "Successfully loaded scene from: " << scenePath << std::endl;
        
        // Verify the scene was loaded correctly
        std::cout << "Loaded scene name: " << loadedScene->getName() << std::endl;
        std::cout << "Number of actors: " << loadedScene->getAllActors().size() << std::endl;
        
        // Count light components instead of legacy lights
        int lightCount = 0;
        for (const auto& [id, actor] : loadedScene->getAllActors()) {
            if (actor->getComponent<LightComponent>()) {
                lightCount++;
            }
        }
        std::cout << "Number of light components: " << lightCount << std::endl;
        
        // Get the first actor
        if (!loadedScene->getAllActors().empty()) {
            auto firstActor = loadedScene->getAllActors().begin()->second;
            std::cout << "First actor name: " << firstActor->getName() << std::endl;
        }
    } else {
        std::cerr << "Failed to load scene!" << std::endl;
    }
}

// Entry point if we want to compile this as a standalone test
#ifdef SCENE_TEST_MAIN
int main() {
    testSceneSerialization();
    return 0;
}
#endif 