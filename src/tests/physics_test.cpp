#include <iostream>
#include <memory>
#include "../core/physics/world/physics_world.hpp"
#include "../core/physics/world/physics_settings.hpp"
#include "../core/physics/dynamics/rigid_body.hpp"
#include "../core/component/physics_component.hpp"

using namespace ohao;

void testRigidBodyPhysics() {
    std::cout << "=== Testing RigidBody Physics (Unit Test) ===" << std::endl;
    
    // Test RigidBody directly without PhysicsWorld
    auto rigidBody = std::make_shared<physics::dynamics::RigidBody>(nullptr);
    
    // Configure the rigid body
    rigidBody->setMass(1.0f);
    rigidBody->setPosition(glm::vec3(0.0f, 10.0f, 0.0f));
    rigidBody->setLinearVelocity(glm::vec3(0.0f, 0.0f, 0.0f));
    rigidBody->setType(physics::dynamics::RigidBodyType::DYNAMIC);
    
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
    physics::PhysicsWorld world;
    physics::PhysicsSettings settings;
    settings.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    
    if (!world.initialize(settings)) {
        std::cout << "❌ Failed to initialize physics world" << std::endl;
        return;
    }
    
    // IMPORTANT: Set simulation state to RUNNING for the test
    world.setSimulationState(physics::SimulationState::RUNNING);
    
    // Create a simple physics component (no transform to avoid crashes)
    auto physicsComp = std::make_unique<ohao::PhysicsComponent>();
    physicsComp->setPhysicsWorld(&world);
    
    // IMPORTANT: Initialize the physics component to create rigid body
    physicsComp->initialize();
    
    // Get the rigid body that was automatically created
    auto rigidBody = physicsComp->getRigidBody();
    if (!rigidBody) {
        std::cout << "❌ Failed to get rigid body from physics component" << std::endl;
        return;
    }
    
    // Configure the rigid body
    rigidBody->setMass(1.0f);
    rigidBody->setPosition(glm::vec3(0.0f, 5.0f, 0.0f));
    rigidBody->setLinearVelocity(glm::vec3(0.0f, 0.0f, 0.0f));
    rigidBody->setType(physics::dynamics::RigidBodyType::DYNAMIC);
    
    // IMPORTANT: Create a collision shape for the rigid body
    physicsComp->createSphereShape(0.5f);
    
    // Create a static ground plane for collision testing
    auto groundPhysicsComp = std::make_unique<ohao::PhysicsComponent>();
    groundPhysicsComp->setPhysicsWorld(&world);
    groundPhysicsComp->initialize();
    
    auto groundRigidBody = groundPhysicsComp->getRigidBody();
    
    if (groundRigidBody) {
        groundRigidBody->setType(physics::dynamics::RigidBodyType::STATIC);
        groundRigidBody->setPosition(glm::vec3(0.0f, -0.1f, 0.0f));
        groundPhysicsComp->createBoxShape(glm::vec3(10.0f, 0.1f, 10.0f)); // Large ground plane
        std::cout << "Ground plane created at Y: -0.1" << std::endl;
    }
    
    // Debug: Check if collision shapes were created
    auto sphereShape = rigidBody->getCollisionShape();
    auto groundShape = groundRigidBody ? groundRigidBody->getCollisionShape() : nullptr;
    
    std::cout << "Sphere collision shape: " << (sphereShape ? "Created" : "NULL") << std::endl;
    std::cout << "Ground collision shape: " << (groundShape ? "Created" : "NULL") << std::endl;
    
    std::cout << "Initial position: (" << 
                 rigidBody->getPosition().x << ", " << 
                 rigidBody->getPosition().y << ", " << 
                 rigidBody->getPosition().z << ")" << std::endl;
    
    // Debug: Check initial physics state
    std::cout << "Initial sphere mass: " << rigidBody->getMass() << std::endl;
    std::cout << "Initial sphere type: " << static_cast<int>(rigidBody->getType()) << std::endl;
    std::cout << "Initial sphere velocity: " << rigidBody->getLinearVelocity().x << ", " << 
                 rigidBody->getLinearVelocity().y << ", " << rigidBody->getLinearVelocity().z << std::endl;
    
    if (groundRigidBody) {
        std::cout << "Ground type: " << static_cast<int>(groundRigidBody->getType()) << std::endl;
        std::cout << "Ground position: " << groundRigidBody->getPosition().x << ", " << 
                     groundRigidBody->getPosition().y << ", " << groundRigidBody->getPosition().z << std::endl;
        
        // Debug: Check AABBs
        auto sphereAABB = rigidBody->getAABB();
        auto groundAABB = groundRigidBody->getAABB();
        
        std::cout << "Sphere AABB: min(" << sphereAABB.min.x << ", " << sphereAABB.min.y << ", " << sphereAABB.min.z << 
                     ") max(" << sphereAABB.max.x << ", " << sphereAABB.max.y << ", " << sphereAABB.max.z << ")" << std::endl;
        std::cout << "Ground AABB: min(" << groundAABB.min.x << ", " << groundAABB.min.y << ", " << groundAABB.min.z << 
                     ") max(" << groundAABB.max.x << ", " << groundAABB.max.y << ", " << groundAABB.max.z << ")" << std::endl;
    }
    
    // Run simulation (this might crash if transform system is called)
    float deltaTime = 1.0f / 60.0f;
    bool collisionDetected = false;
    
    for (int frame = 0; frame < 120; ++frame) { // 2 seconds
        try {
            glm::vec3 posBeforeStep = rigidBody->getPosition();
            world.stepSimulation(deltaTime);
            glm::vec3 posAfterStep = rigidBody->getPosition();
            
            // Check if sphere stopped falling (collision occurred)
            if (frame > 10 && abs(posAfterStep.y - posBeforeStep.y) < 0.001f && posAfterStep.y > -1.0f) {
                collisionDetected = true;
                std::cout << "✅ Collision detected at frame " << frame << std::endl;
                std::cout << "Final sphere position: " << posAfterStep.y << std::endl;
            }
            
            if (frame % 15 == 0) { // Every 0.25 seconds
                glm::vec3 pos = rigidBody->getPosition();
                glm::vec3 vel = rigidBody->getLinearVelocity();
                std::cout << "Frame " << frame << " - Position Y: " << pos.y << 
                           " - Velocity Y: " << vel.y << std::endl;
                
                // Debug: Check if sphere is falling through ground
                if (pos.y < -2.0f) {
                    std::cout << "❌ Sphere fell through ground - collision detection failed" << std::endl;
                    break;
                }
            }
        } catch (...) {
            std::cout << "❌ Crash during simulation at frame " << frame << std::endl;
            return;
        }
    }
    
    glm::vec3 finalPos = rigidBody->getPosition();
    if (collisionDetected || (finalPos.y < 5.0f && finalPos.y > -1.0f)) {
        std::cout << "✅ SUCCESS: PhysicsWorld integration working!" << std::endl;
    } else {
        std::cout << "❌ FAILURE: Object didn't behave correctly (final Y: " << finalPos.y << ")" << std::endl;
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