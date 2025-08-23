#include <iostream>
#include <memory>
#include <string>

#include "engine/actor/actor.hpp"
#include "engine/component/component.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/scene/scene.hpp"

using namespace ohao;

// Simple test to demonstrate the actor-component system
void runActorComponentTest() {
    std::cout << "Testing Actor-Component System...\n";
    
    // Create a scene
    auto scene = std::make_shared<Scene>("Test Scene");
    
    // Create a few actors
    auto rootActor = scene->createActor("Root");
    auto childActor1 = scene->createActor("Child1");
    auto childActor2 = scene->createActor("Child2");
    
    // Set up hierarchy
    childActor1->setParent(rootActor.get());
    childActor2->setParent(rootActor.get());
    
    // Add components
    auto meshComp1 = childActor1->addComponent<MeshComponent>();
    auto physicsComp1 = childActor1->addComponent<PhysicsComponent>();
    
    auto meshComp2 = childActor2->addComponent<MeshComponent>();
    auto physicsComp2 = childActor2->addComponent<PhysicsComponent>();
    
    // Configure physics
    physicsComp1->setMass(10.0f);
    physicsComp1->createBoxShape(glm::vec3(1.0f, 1.0f, 1.0f));
    
    physicsComp2->setMass(5.0f);
    physicsComp2->createSphereShape(0.5f);
    
    // Set up transforms
    rootActor->getTransform()->setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    
    childActor1->getTransform()->setPosition(glm::vec3(2.0f, 0.0f, 0.0f));
    childActor1->getTransform()->setRotationEuler(glm::vec3(0.0f, 45.0f, 0.0f));
    
    childActor2->getTransform()->setPosition(glm::vec3(-2.0f, 0.0f, 0.0f));
    childActor2->getTransform()->setScale(glm::vec3(0.5f, 0.5f, 0.5f));
    
    // Output actor hierarchy
    std::cout << "Scene: " << scene->getName() << std::endl;
    std::cout << "Root: " << rootActor->getName() << std::endl;
    
    for (auto child : rootActor->getChildren()) {
        std::cout << "  Child: " << child->getName() << std::endl;
        
        // List components
        std::cout << "    Components:" << std::endl;
        for (auto& comp : child->getAllComponents()) {
            std::cout << "      - " << comp->getTypeName() << std::endl;
        }
        
        // Show transform
        auto transform = child->getTransform();
        auto pos = transform->getPosition();
        auto rot = transform->getRotationEuler();
        auto scale = transform->getScale();
        
        std::cout << "    Transform: " << std::endl;
        std::cout << "      Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
        std::cout << "      Rotation: (" << rot.x << ", " << rot.y << ", " << rot.z << ")" << std::endl;
        std::cout << "      Scale: (" << scale.x << ", " << scale.y << ", " << scale.z << ")" << std::endl;
    }
    
    // Simulate a few physics steps
    std::cout << "\nSimulating physics...\n";
    
    // Apply forces
    physicsComp1->applyForce(glm::vec3(0.0f, 10.0f, 0.0f));
    physicsComp2->applyForce(glm::vec3(0.0f, 5.0f, 0.0f));
    
    // Update the scene a few times
    for (int i = 0; i < 10; i++) {
        scene->update(0.016f); // ~60 FPS
        
        // Output positions after each step
        std::cout << "Step " << i << ":" << std::endl;
        for (auto child : rootActor->getChildren()) {
            auto pos = child->getTransform()->getPosition();
            std::cout << "  " << child->getName() << " position: (" 
                     << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
            
            // Get physics component
            auto physics = child->getComponent<PhysicsComponent>();
            if (physics) {
                auto vel = physics->getLinearVelocity();
                std::cout << "  " << child->getName() << " velocity: (" 
                         << vel.x << ", " << vel.y << ", " << vel.z << ")" << std::endl;
            }
        }
    }
    
    std::cout << "Actor-Component System Test Completed\n";
}

// Can be called from main
#ifdef RUN_ACTOR_COMPONENT_TEST
int main() {
    runActorComponentTest();
    return 0;
}
#endif 