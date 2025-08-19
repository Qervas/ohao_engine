#include "component_factory.hpp"
#include "core/scene/scene.hpp"
#include "core/asset/model.hpp"
#include "ui/components/console_widget.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace ohao {

std::shared_ptr<Actor> ComponentFactory::createActorWithComponents(
    Scene* scene, 
    const std::string& name, 
    PrimitiveType type
) {
    if (!scene) {
        OHAO_LOG_ERROR("Cannot create actor: scene is null");
        return nullptr;
    }
    
    // Create the actor
    auto actor = scene->createActor(name);
    if (!actor) {
        OHAO_LOG_ERROR("Failed to create actor: " + name);
        return nullptr;
    }
    
    // Add components based on type
    if (!addComponentsToActor(actor, type)) {
        OHAO_LOG_ERROR("Failed to add components to actor: " + name);
        return nullptr;
    }
    
    // Connect dependencies and initialize
    ComponentManager::connectComponentDependencies(actor);
    ComponentManager::initializeComponents(actor);
    
    OHAO_LOG("Created actor '" + name + "' with automatic components for type: " + std::to_string(static_cast<int>(type)));
    return actor;
}

bool ComponentFactory::addComponentsToActor(std::shared_ptr<Actor> actor, PrimitiveType type) {
    if (!actor) return false;
    
    ComponentSet config = getComponentSet(type);
    
    // Always add transform (actors should have this by default, but ensure it)
    if (!actor->getTransform()) {
        OHAO_LOG_WARNING("Actor missing transform component, this shouldn't happen");
        return false;
    }
    
    // Add mesh component if needed
    if (config.needsMesh) {
        auto meshComponent = actor->addComponent<MeshComponent>();
        if (meshComponent) {
            setupMeshComponent(meshComponent.get(), type);
        } else {
            OHAO_LOG_ERROR("Failed to add mesh component");
            return false;
        }
    }
    
    // Add physics component if needed
    if (config.needsPhysics) {
        auto physicsComponent = actor->addComponent<PhysicsComponent>();
        if (physicsComponent) {
            setupPhysicsComponent(physicsComponent.get(), config, type);
        } else {
            OHAO_LOG_ERROR("Failed to add physics component");
            return false;
        }
    }
    
    // Add material component if needed
    if (config.needsMaterial) {
        auto materialComponent = actor->addComponent<MaterialComponent>();
        if (materialComponent) {
            setupMaterialComponent(materialComponent.get(), config);
        } else {
            OHAO_LOG_ERROR("Failed to add material component");
            return false;
        }
    }
    
    // Add light component if needed
    if (config.needsLight) {
        auto lightComponent = actor->addComponent<LightComponent>();
        if (lightComponent) {
            setupLightComponent(lightComponent.get(), config);
        } else {
            OHAO_LOG_ERROR("Failed to add light component");
            return false;
        }
    }
    
    return true;
}

ComponentSet ComponentFactory::getComponentSet(PrimitiveType type) {
    ComponentSet config;
    
    switch (type) {
        case PrimitiveType::Cube:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::DYNAMIC;
            config.mass = 1.0f;
            config.materialColor = glm::vec3(0.7f, 0.7f, 0.8f);
            break;
            
        case PrimitiveType::Sphere:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::DYNAMIC;
            config.mass = 1.0f;
            config.materialColor = glm::vec3(0.6f, 0.7f, 0.8f);
            break;
            
        case PrimitiveType::Plane:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::STATIC;
            config.mass = 0.0f;
            config.friction = 0.8f;
            config.restitution = 0.2f;
            config.materialColor = glm::vec3(0.4f, 0.6f, 0.4f);
            break;
            
        case PrimitiveType::Cylinder:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::DYNAMIC;
            config.mass = 1.5f;
            config.materialColor = glm::vec3(0.8f, 0.6f, 0.7f);
            break;
            
        case PrimitiveType::Cone:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::DYNAMIC;
            config.mass = 0.8f;
            config.materialColor = glm::vec3(0.7f, 0.8f, 0.6f);
            break;
            
        case PrimitiveType::PointLight:
            config.needsLight = true;
            config.lightType = LightType::Point;
            config.intensity = 1.0f;
            config.lightColor = glm::vec3(1.0f, 1.0f, 1.0f);
            break;
            
        case PrimitiveType::DirectionalLight:
            config.needsLight = true;
            config.lightType = LightType::Directional;
            config.intensity = 3.0f;
            config.lightColor = glm::vec3(1.0f, 1.0f, 0.9f);
            break;
            
        case PrimitiveType::SpotLight:
            config.needsLight = true;
            config.lightType = LightType::Spot;
            config.intensity = 2.0f;
            config.lightColor = glm::vec3(1.0f, 0.9f, 0.8f);
            break;
            
        case PrimitiveType::Empty:
        default:
            // Empty object - only has transform
            break;
    }
    
    return config;
}

void ComponentFactory::setupMeshComponent(MeshComponent* mesh, PrimitiveType type) {
    auto model = generateMeshForPrimitive(type);
    if (model) {
        mesh->setModel(model);
        OHAO_LOG("Setup mesh component for primitive type: " + std::to_string(static_cast<int>(type)));
    } else {
        OHAO_LOG_ERROR("Failed to generate mesh for primitive type: " + std::to_string(static_cast<int>(type)));
    }
}

void ComponentFactory::setupPhysicsComponent(PhysicsComponent* physics, const ComponentSet& config, PrimitiveType type) {
    // Set physics properties
    physics->setMass(config.mass);
    physics->setRigidBodyType(config.physicsType);
    physics->setFriction(config.friction);
    physics->setRestitution(config.restitution);
    
    // Setup collision shape
    setupPhysicsShape(physics, type);
    
    OHAO_LOG("Setup physics component with mass: " + std::to_string(config.mass) + 
             ", type: " + std::to_string(static_cast<int>(config.physicsType)));
}

void ComponentFactory::setupPhysicsShape(PhysicsComponent* physics, PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Cube:
            physics->createBoxShape(glm::vec3(0.5f, 0.5f, 0.5f));
            break;
            
        case PrimitiveType::Sphere:
            physics->createSphereShape(0.5f);
            break;
            
        case PrimitiveType::Plane:
            physics->createBoxShape(glm::vec3(5.0f, 0.1f, 5.0f)); // Large thin box
            break;
            
        case PrimitiveType::Cylinder:
            // TODO: Implement CapsuleShape and createCapsuleShape method
            // physics->createCapsuleShape(0.5f, 1.0f);
            physics->createBoxShape(0.5f, 1.0f, 0.5f); // Use box as temporary fallback
            break;
            
        case PrimitiveType::Cone:
            physics->createBoxShape(glm::vec3(0.5f, 0.5f, 0.5f)); // Approximate with box for now
            break;
            
        default:
            physics->createBoxShape(glm::vec3(0.5f, 0.5f, 0.5f));
            break;
    }
}

void ComponentFactory::setupMaterialComponent(MaterialComponent* material, const ComponentSet& config) {
    Material mat;
    mat.baseColor = config.materialColor;
    mat.roughness = config.roughness;
    mat.metallic = config.metallic;
    mat.ao = 1.0f;
    mat.name = "Auto-Generated Material";
    
    material->setMaterial(mat);
    OHAO_LOG("Setup material component with color: (" + 
             std::to_string(config.materialColor.x) + ", " + 
             std::to_string(config.materialColor.y) + ", " + 
             std::to_string(config.materialColor.z) + ")");
}

void ComponentFactory::setupLightComponent(LightComponent* light, const ComponentSet& config) {
    light->setLightType(config.lightType);
    light->setColor(config.lightColor);
    light->setIntensity(config.intensity);
    
    // Setup type-specific properties
    switch (config.lightType) {
        case LightType::Point:
            light->setRange(10.0f);
            break;
            
        case LightType::Directional:
            light->setDirection(glm::vec3(0.2f, -1.0f, 0.3f));
            break;
            
        case LightType::Spot:
            light->setDirection(glm::vec3(0.0f, -1.0f, 0.0f));
            light->setRange(15.0f);
            light->setInnerConeAngle(30.0f);
            light->setOuterConeAngle(45.0f);
            break;
    }
    
    OHAO_LOG("Setup light component with type: " + std::to_string(static_cast<int>(config.lightType)));
}

std::shared_ptr<Model> ComponentFactory::generateMeshForPrimitive(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Cube:
            return generateCubeMesh();
        case PrimitiveType::Sphere:
            return generateSphereMesh();
        case PrimitiveType::Plane:
            return generatePlaneMesh();
        case PrimitiveType::Cylinder:
            return generateCylinderMesh();
        case PrimitiveType::Cone:
            return generateConeMesh();
        default:
            return nullptr;
    }
}

// Mesh generation implementations
std::shared_ptr<Model> ComponentFactory::generateCubeMesh() {
    auto model = std::make_shared<Model>();
    
    const float size = 1.0f;
    const float hs = size * 0.5f; // half size

    // Vertices for a cube with proper normals and UVs
    std::vector<Vertex> vertices = {
        // Front face
        {{-hs, -hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {0, 0}}, // 0
        {{ hs, -hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {1, 0}}, // 1
        {{ hs,  hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {1, 1}}, // 2
        {{-hs,  hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {0, 1}}, // 3

        // Back face
        {{ hs, -hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {0, 0}}, // 4
        {{-hs, -hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {1, 0}}, // 5
        {{-hs,  hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {1, 1}}, // 6
        {{ hs,  hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {0, 1}}, // 7

        // Top face
        {{-hs,  hs, -hs}, {1, 1, 1}, { 0,  1,  0}, {0, 0}}, // 8
        {{ hs,  hs, -hs}, {1, 1, 1}, { 0,  1,  0}, {1, 0}}, // 9
        {{ hs,  hs,  hs}, {1, 1, 1}, { 0,  1,  0}, {1, 1}}, // 10
        {{-hs,  hs,  hs}, {1, 1, 1}, { 0,  1,  0}, {0, 1}}, // 11

        // Bottom face
        {{-hs, -hs, -hs}, {1, 1, 1}, { 0, -1,  0}, {0, 0}}, // 12
        {{ hs, -hs, -hs}, {1, 1, 1}, { 0, -1,  0}, {1, 0}}, // 13
        {{ hs, -hs,  hs}, {1, 1, 1}, { 0, -1,  0}, {1, 1}}, // 14
        {{-hs, -hs,  hs}, {1, 1, 1}, { 0, -1,  0}, {0, 1}}, // 15

        // Right face
        {{ hs, -hs,  hs}, {1, 1, 1}, { 1,  0,  0}, {0, 0}}, // 16
        {{ hs, -hs, -hs}, {1, 1, 1}, { 1,  0,  0}, {1, 0}}, // 17
        {{ hs,  hs, -hs}, {1, 1, 1}, { 1,  0,  0}, {1, 1}}, // 18
        {{ hs,  hs,  hs}, {1, 1, 1}, { 1,  0,  0}, {0, 1}}, // 19

        // Left face
        {{-hs, -hs, -hs}, {1, 1, 1}, {-1,  0,  0}, {0, 0}}, // 20
        {{-hs, -hs,  hs}, {1, 1, 1}, {-1,  0,  0}, {1, 0}}, // 21
        {{-hs,  hs,  hs}, {1, 1, 1}, {-1,  0,  0}, {1, 1}}, // 22
        {{-hs,  hs, -hs}, {1, 1, 1}, {-1,  0,  0}, {0, 1}}  // 23
    };

    // Indices for the cube
    std::vector<uint32_t> indices = {
        0,  1,  2,  2,  3,  0,  // Front
        4,  5,  6,  6,  7,  4,  // Back
        8,  9,  10, 10, 11, 8,  // Top
        12, 13, 14, 14, 15, 12, // Bottom
        16, 17, 18, 18, 19, 16, // Right
        20, 21, 22, 22, 23, 20  // Left
    };

    model->vertices = vertices;
    model->indices = indices;
    
    return model;
}

std::shared_ptr<Model> ComponentFactory::generateSphereMesh() {
    auto model = std::make_shared<Model>();
    
    const float radius = 0.5f;
    const int sectors = 32;  // longitude
    const int stacks = 16;   // latitude

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Generate vertices
    for (int i = 0; i <= stacks; ++i) {
        float phi = glm::pi<float>() * float(i) / float(stacks);
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);

        for (int j = 0; j <= sectors; ++j) {
            float theta = 2.0f * glm::pi<float>() * float(j) / float(sectors);
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);

            float x = cosTheta * sinPhi;
            float y = cosPhi;
            float z = sinTheta * sinPhi;

            Vertex vertex;
            vertex.position = {x * radius, y * radius, z * radius};
            vertex.normal = {x, y, z};  // Normalized position = normal for sphere
            vertex.color = {1.0f, 1.0f, 1.0f};
            vertex.texCoord = {float(j) / sectors, float(i) / stacks};

            vertices.push_back(vertex);
        }
    }

    // Generate indices
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < sectors; ++j) {
            int first = i * (sectors + 1) + j;
            int second = first + sectors + 1;

            indices.push_back(first);
            indices.push_back(second);
            indices.push_back(first + 1);

            indices.push_back(second);
            indices.push_back(second + 1);
            indices.push_back(first + 1);
        }
    }

    model->vertices = vertices;
    model->indices = indices;
    
    return model;
}

std::shared_ptr<Model> ComponentFactory::generatePlaneMesh(float size) {
    auto model = std::make_shared<Model>();
    
    const int subdivisions = 1;  // Increase for more detailed plane
    const float step = size / subdivisions;
    const float uvStep = 1.0f / subdivisions;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Generate vertices
    for (int i = 0; i <= subdivisions; ++i) {
        for (int j = 0; j <= subdivisions; ++j) {
            float x = -size/2 + j * step;
            float z = -size/2 + i * step;

            Vertex vertex;
            vertex.position = {x, 0.0f, z};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.color = {1.0f, 1.0f, 1.0f};
            vertex.texCoord = {j * uvStep, i * uvStep};

            vertices.push_back(vertex);
        }
    }

    // Generate indices
    for (int i = 0; i < subdivisions; ++i) {
        for (int j = 0; j < subdivisions; ++j) {
            int row1 = i * (subdivisions + 1);
            int row2 = (i + 1) * (subdivisions + 1);

            // Triangle 1
            indices.push_back(row1 + j);
            indices.push_back(row1 + j + 1);
            indices.push_back(row2 + j + 1);

            // Triangle 2
            indices.push_back(row1 + j);
            indices.push_back(row2 + j + 1);
            indices.push_back(row2 + j);
        }
    }

    model->vertices = vertices;
    model->indices = indices;
    
    return model;
}

std::shared_ptr<Model> ComponentFactory::generateCylinderMesh() {
    // For now, return a simple cylinder approximation using a box
    // TODO: Implement proper cylinder mesh generation
    return generateCubeMesh();
}

std::shared_ptr<Model> ComponentFactory::generateConeMesh() {
    // For now, return a simple cone approximation using a box
    // TODO: Implement proper cone mesh generation
    return generateCubeMesh();
}

// ComponentManager implementation
void ComponentManager::connectComponentDependencies(std::shared_ptr<Actor> actor) {
    if (!actor) return;
    
    connectPhysicsToTransform(actor);
    connectMeshToMaterial(actor);
    
    OHAO_LOG("Connected component dependencies for actor: " + actor->getName());
}

void ComponentManager::connectPhysicsToTransform(std::shared_ptr<Actor> actor) {
    auto physicsComponent = actor->getComponent<PhysicsComponent>();
    auto transformComponent = actor->getTransform();
    
    if (physicsComponent && transformComponent) {
        physicsComponent->setTransformComponent(transformComponent);
        OHAO_LOG("Connected physics component to transform");
    }
}

void ComponentManager::connectMeshToMaterial(std::shared_ptr<Actor> actor) {
    // For now, mesh and material are connected through the rendering system
    // This could be extended for more complex material binding
}

void ComponentManager::initializeComponents(std::shared_ptr<Actor> actor) {
    if (!actor) return;
    
    // Initialize the actor (which initializes all its components)
    actor->initialize();
    
    OHAO_LOG("Initialized all components for actor: " + actor->getName());
}

bool ComponentManager::validateComponentSetup(std::shared_ptr<Actor> actor, PrimitiveType type) {
    if (!actor) return false;
    
    ComponentSet expectedConfig = ComponentFactory::getComponentSet(type);
    
    // Check required components
    if (expectedConfig.needsMesh && !actor->getComponent<MeshComponent>()) {
        OHAO_LOG_ERROR("Actor missing required mesh component");
        return false;
    }
    
    if (expectedConfig.needsPhysics && !actor->getComponent<PhysicsComponent>()) {
        OHAO_LOG_ERROR("Actor missing required physics component");
        return false;
    }
    
    if (expectedConfig.needsMaterial && !actor->getComponent<MaterialComponent>()) {
        OHAO_LOG_ERROR("Actor missing required material component");
        return false;
    }
    
    if (expectedConfig.needsLight && !actor->getComponent<LightComponent>()) {
        OHAO_LOG_ERROR("Actor missing required light component");
        return false;
    }
    
    OHAO_LOG("Component setup validation passed for actor: " + actor->getName());
    return true;
}

} // namespace ohao