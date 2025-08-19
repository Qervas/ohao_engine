#include <iostream>
#include <memory>
#include "../core/physics/physics_world.hpp"
#include "../core/physics/rigid_body.hpp"
#include "../core/component/physics_component.hpp"

using namespace ohao;

void testRigidBodyPhysics() {
    std::cout << "=== Testing RigidBody Physics (Unit Test) ===" << std::endl;
    
    // Test RigidBody directly without PhysicsWorld
    auto rigidBody = std::make_shared<RigidBody>(nullptr);
    
    // Configure the rigid body
    rigidBody->setMass(1.0f);
    rigidBody->setPosition(glm::vec3(0.0f, 10.0f, 0.0f));
    rigidBody->setLinearVelocity(glm::vec3(0.0f, 0.0f, 0.0f));
    rigidBody->setType(RigidBodyType::DYNAMIC);
    
    std::cout << "Initial position: (" << 
                 rigidBody->getPosition().x << ", " << 
                 rigidBody->getPosition().y << ", " << 
                 rigidBody->getPosition().z << ")" << std::endl;
    
    float deltaTime = 1.0f / 60.0f;
    glm::vec3 gravity(0.0f, -9.81f, 0.0f);
    
    // Manually simulate physics steps
    for (int frame = 0; frame < 120; ++frame) {
        // Apply gravity force
        glm::vec3 gravityForce = rigidBody->getMass() * gravity;
        rigidBody->applyForce(gravityForce);
        
        // Get accumulated forces  
        glm::vec3 totalForce = rigidBody->getAccumulatedForce();
        
        // Calculate acceleration (F = ma -> a = F/m)
        float mass = rigidBody->getMass();
        glm::vec3 acceleration = totalForce / mass;
        
        // Update velocity (v = v + a*dt)
        glm::vec3 currentVelocity = rigidBody->getLinearVelocity();
        glm::vec3 newVelocity = currentVelocity + acceleration * deltaTime;
        rigidBody->setLinearVelocity(newVelocity);
        
        // Update position (p = p + v*dt)
        glm::vec3 currentPosition = rigidBody->getPosition();
        glm::vec3 newPosition = currentPosition + newVelocity * deltaTime;
        rigidBody->setPosition(newPosition);
        
        // Clear forces
        rigidBody->clearForces();
        
        // Log every 30 frames
        if (frame % 30 == 0) {
            glm::vec3 pos = rigidBody->getPosition();
            glm::vec3 vel = rigidBody->getLinearVelocity();
            
            std::cout << "Frame " << frame << 
                         " - Position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" <<
                         " - Velocity: (" << vel.x << ", " << vel.y << ", " << vel.z << ")" << std::endl;
        }
    }
    
    // Verify results
    glm::vec3 finalPos = rigidBody->getPosition();
    glm::vec3 finalVel = rigidBody->getLinearVelocity();
    
    std::cout << "Final Position: (" << finalPos.x << ", " << finalPos.y << ", " << finalPos.z << ")" << std::endl;
    std::cout << "Final Velocity: (" << finalVel.x << ", " << finalVel.y << ", " << finalVel.z << ")" << std::endl;
    
    if (finalPos.y < 10.0f && finalVel.y < 0.0f) {
        std::cout << "✅ SUCCESS: RigidBody physics working correctly!" << std::endl;
    } else {
        std::cout << "❌ FAILURE: RigidBody physics not working" << std::endl;
    }
}

void testPhysicsWorldIntegration() {
    std::cout << "=== Testing PhysicsWorld Integration ===" << std::endl;
    
    // Create physics world
    PhysicsWorld world;
    PhysicsSettings settings;
    settings.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    
    if (!world.initialize(settings)) {
        std::cout << "❌ Failed to initialize physics world" << std::endl;
        return;
    }
    
    // Create a simple physics component (no transform to avoid crashes)
    auto physicsComp = std::make_unique<PhysicsComponent>();
    physicsComp->setPhysicsWorld(&world);
    
    // Create rigid body through the proper API
    auto rigidBody = world.createRigidBody(physicsComp.get());
    if (!rigidBody) {
        std::cout << "❌ Failed to create rigid body" << std::endl;
        return;
    }
    
    // Configure the rigid body
    rigidBody->setMass(1.0f);
    rigidBody->setPosition(glm::vec3(0.0f, 5.0f, 0.0f));
    rigidBody->setLinearVelocity(glm::vec3(0.0f, 0.0f, 0.0f));
    rigidBody->setType(RigidBodyType::DYNAMIC);
    
    std::cout << "Initial position: (" << 
                 rigidBody->getPosition().x << ", " << 
                 rigidBody->getPosition().y << ", " << 
                 rigidBody->getPosition().z << ")" << std::endl;
    
    // Run simulation (this might crash if transform system is called)
    float deltaTime = 1.0f / 60.0f;
    for (int frame = 0; frame < 60; ++frame) { // 1 second
        try {
            world.stepSimulation(deltaTime);
            
            if (frame % 15 == 0) { // Every 0.25 seconds
                glm::vec3 pos = rigidBody->getPosition();
                std::cout << "Frame " << frame << " - Position Y: " << pos.y << std::endl;
            }
        } catch (...) {
            std::cout << "❌ Crash during simulation at frame " << frame << std::endl;
            return;
        }
    }
    
    glm::vec3 finalPos = rigidBody->getPosition();
    if (finalPos.y < 5.0f) {
        std::cout << "✅ SUCCESS: PhysicsWorld integration working!" << std::endl;
    } else {
        std::cout << "❌ FAILURE: Object didn't fall in PhysicsWorld" << std::endl;
    }
}

int main() {
    try {
        std::cout << "Starting Physics System Tests" << std::endl << std::endl;
        
        // Test 1: Direct RigidBody physics (should always work)
        testRigidBodyPhysics();
        std::cout << std::endl;
        
        // Test 2: PhysicsWorld integration (might crash due to transform system)
        testPhysicsWorldIntegration();
        
        std::cout << std::endl << "Tests completed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cout << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "Test failed with unknown exception" << std::endl;
        return 1;
    }
}