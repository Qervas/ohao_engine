#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/light_component.hpp"
#include "engine/serialization/scene_serializer.hpp"
#include <iostream>
#include <memory>

using namespace ohao;

int main() {
    std::cout << "Testing Scene Serialization/Deserialization" << std::endl;
    
    // Create a test scene
    auto scene = std::make_unique<Scene>("Test Scene");
    
    // Add some actors
    auto cube = std::make_shared<Actor>("Cube");
    auto sphere = std::make_shared<Actor>("Sphere");
    
    // Position them
    cube->getTransform()->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    sphere->getTransform()->setPosition(glm::vec3(3.0f, 0.0f, 0.0f));
    
    // Add them to the scene
    scene->addActor(cube);
    scene->addActor(sphere);
    
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
    desc.createdBy = "Simple Serialization Test";
    desc.lastModified = "0";  // Will be updated during save
    scene->setDescriptor(desc);
    
    // Create a serializer
    SceneSerializer serializer(scene.get());
    
    // Save the scene to a file
    std::string scenePath = "test_scenes/simple_test_scene.ohscene";
    if (serializer.serialize(scenePath)) {
        std::cout << "Successfully saved scene to: " << scenePath << std::endl;
    } else {
        std::cerr << "Failed to save scene!" << std::endl;
        return 1;
    }
    
    // Create a new scene to load into
    auto loadedScene = std::make_unique<Scene>();
    SceneSerializer loadSerializer(loadedScene.get());
    
    // Load the scene from the file
    if (loadSerializer.deserialize(scenePath)) {
        std::cout << "Successfully loaded scene from: " << scenePath << std::endl;
        
        // Verify the scene was loaded correctly
        std::cout << "Loaded scene name: " << loadedScene->getName() << std::endl;
        std::cout << "Number of actors: " << loadedScene->getAllActors().size() - 1 << " (excluding root)" << std::endl;
        
        // Count light components instead of legacy lights
        int lightCount = 0;
        for (const auto& [id, actor] : loadedScene->getAllActors()) {
            if (actor->getComponent<LightComponent>()) {
                lightCount++;
            }
        }
        std::cout << "Number of light components: " << lightCount << std::endl;
        
        // Get the actors
        auto cubeLoaded = loadedScene->findActor("Cube");
        auto sphereLoaded = loadedScene->findActor("Sphere");
        
        if (cubeLoaded && sphereLoaded) {
            std::cout << "Found both actors in the loaded scene" << std::endl;
            std::cout << "Cube position: " 
                      << cubeLoaded->getTransform()->getPosition().x << ", "
                      << cubeLoaded->getTransform()->getPosition().y << ", "
                      << cubeLoaded->getTransform()->getPosition().z << std::endl;
            std::cout << "Sphere position: " 
                      << sphereLoaded->getTransform()->getPosition().x << ", "
                      << sphereLoaded->getTransform()->getPosition().y << ", "
                      << sphereLoaded->getTransform()->getPosition().z << std::endl;
        } else {
            std::cerr << "Could not find actors in the loaded scene!" << std::endl;
        }
    } else {
        std::cerr << "Failed to load scene!" << std::endl;
        return 1;
    }
    
    return 0;
} 