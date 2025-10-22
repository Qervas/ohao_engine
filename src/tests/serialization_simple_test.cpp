#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"
#include "renderer/components/light_component.hpp"
#include "engine/serialization/map_io.hpp"
#include <iostream>
#include <memory>

using namespace ohao;

void testSceneSerialization() {
    std::cout << "Testing Scene Serialization/Deserialization" << std::endl;
    
    auto scene = std::make_unique<Scene>("Test Scene");
    
    auto cube = scene->createActor("Cube");
    auto meshComp = cube->addComponent<MeshComponent>();
    auto physicsComp = cube->addComponent<PhysicsComponent>();
    physicsComp->createBoxShape(glm::vec3(1.0f, 1.0f, 1.0f));
    
    auto sphere = scene->createActor("Sphere");
    sphere->getTransform()->setPosition(glm::vec3(3.0f, 0.0f, 0.0f));
    auto sphereMesh = sphere->addComponent<MeshComponent>();
    auto spherePhysics = sphere->addComponent<PhysicsComponent>();
    spherePhysics->createSphereShape(1.0f);
    
    auto lightActor = scene->createActor("TestLight");
    auto lightComponent = lightActor->addComponent<LightComponent>();
    lightComponent->setLightType(LightType::Point);
    lightComponent->setPosition(glm::vec3(5.0f, 5.0f, 5.0f));
    lightComponent->setColor(glm::vec3(1.0f, 0.9f, 0.8f));
    lightComponent->setIntensity(1.5f);
    
    MapIO serializer(scene.get());
    std::string scenePath = "test_scenes/test_scene.ohmap";
    if (serializer.save(scenePath)) {
        std::cout << "Successfully saved scene to: " << scenePath << std::endl;
    } else {
        std::cerr << "Failed to save scene!" << std::endl;
        return;
    }
    
    auto loadedScene = std::make_unique<Scene>();
    MapIO loadSerializer(loadedScene.get());
    if (loadSerializer.load(scenePath)) {
        std::cout << "Successfully loaded scene from: " << scenePath << std::endl;
        std::cout << "Loaded scene name: " << loadedScene->getName() << std::endl;
        std::cout << "Number of actors: " << loadedScene->getAllActors().size() << std::endl;
    } else {
        std::cerr << "Failed to load scene!" << std::endl;
    }
}

#ifdef SCENE_TEST_MAIN
int main() {
    testSceneSerialization();
    return 0;
}
#endif 