#pragma once

#include "core/actor/actor.hpp"
#include "core/component/mesh_component.hpp"
#include "core/component/physics_component.hpp"
#include "core/component/material_component.hpp"
#include "core/component/light_component.hpp"
#include "core/component/transform_component.hpp"
#include "core/physics/dynamics/rigid_body.hpp"
#include <memory>
#include <glm/glm.hpp>

namespace ohao {

/**
 * Primitive types that can be created with automatic component setup
 */
enum class PrimitiveType {
    Empty,
    Cube,
    Sphere,
    Platform,
    Cylinder,
    Cone,
    PointLight,
    DirectionalLight,
    SpotLight
};

/**
 * Component configuration for different primitive types
 */
struct ComponentSet {
    bool needsMesh = false;
    bool needsPhysics = false;
    bool needsMaterial = false;
    bool needsLight = false;
    
    // Physics settings
    physics::dynamics::RigidBodyType physicsType = physics::dynamics::RigidBodyType::DYNAMIC;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.3f;
    
    // Default material settings
    glm::vec3 materialColor = glm::vec3(0.7f, 0.7f, 0.8f);
    float roughness = 0.5f;
    float metallic = 0.0f;
    
    // Light settings (if needed)
    LightType lightType = LightType::Point;
    glm::vec3 lightColor = glm::vec3(1.0f);
    float intensity = 1.0f;
};

/**
 * Factory for creating components automatically based on primitive type
 */
class ComponentFactory {
public:
    /**
     * Create an actor with appropriate components for the given primitive type
     */
    static std::shared_ptr<Actor> createActorWithComponents(
        Scene* scene, 
        const std::string& name, 
        PrimitiveType type
    );
    
    /**
     * Add default components to an existing actor based on primitive type
     */
    static bool addComponentsToActor(
        std::shared_ptr<Actor> actor, 
        PrimitiveType type
    );
    
    /**
     * Get the component configuration for a primitive type
     */
    static ComponentSet getComponentSet(PrimitiveType type);
    
    /**
     * Generate appropriate mesh for the primitive type
     */
    static std::shared_ptr<Model> generateMeshForPrimitive(PrimitiveType type);
    
    /**
     * Create appropriate collision shape for physics component
     */
    static void setupPhysicsShape(PhysicsComponent* physics, PrimitiveType type);
    
    /**
     * Create triangle mesh collision shape from MeshComponent
     */
    static void setupPhysicsShapeFromMesh(PhysicsComponent* physics, MeshComponent* mesh);

private:
    // Mesh generation helpers
    static std::shared_ptr<Model> generateCubeMesh();
    static std::shared_ptr<Model> generateSphereMesh();
    static std::shared_ptr<Model> generatePlatformMesh(float width = 2.0f, float height = 0.4f, float depth = 2.0f);
    static std::shared_ptr<Model> generateCylinderMesh();
    static std::shared_ptr<Model> generateConeMesh();
    
    // Component setup helpers
    static void setupMeshComponent(MeshComponent* mesh, PrimitiveType type);
    static void setupPhysicsComponent(PhysicsComponent* physics, const ComponentSet& config, PrimitiveType type);
    static void setupMaterialComponent(MaterialComponent* material, const ComponentSet& config);
    static void setupLightComponent(LightComponent* light, const ComponentSet& config);
};

/**
 * Manager for component lifecycle and dependencies
 */
class ComponentManager {
public:
    /**
     * Ensure all component dependencies are properly connected
     */
    static void connectComponentDependencies(std::shared_ptr<Actor> actor);
    
    /**
     * Initialize all components on an actor
     */
    static void initializeComponents(std::shared_ptr<Actor> actor);
    
    /**
     * Validate that all required components are present and properly configured
     */
    static bool validateComponentSetup(std::shared_ptr<Actor> actor, PrimitiveType type);
    
private:
    static void connectPhysicsToTransform(std::shared_ptr<Actor> actor);
    static void connectMeshToMaterial(std::shared_ptr<Actor> actor);
};

} // namespace ohao