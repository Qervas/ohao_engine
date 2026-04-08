#include "component_factory.hpp"
#include "component_pack.hpp"
#include "engine/scene/scene.hpp"
#include "engine/asset/model.hpp"
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
    
    // Use appropriate component pack based on primitive type
    ComponentSet config = getComponentSet(type);
    
    if (config.needsLight) {
        // Light objects (DirectionalLight, PointLight, SpotLight)
        LightOnlyPack::applyTo(actor);
        OHAO_LOG("Applied LightOnlyPack to light primitive '" + actor->getName() + "'");
        
        // Setup light component
        auto lightComponent = actor->getComponent<LightComponent>();
        setupLightComponent(lightComponent.get(), config);
    } else {
        // Standard objects (Cube, Sphere, Platform, etc.)
        StandardObjectPack::applyTo(actor);
        OHAO_LOG("Applied StandardObjectPack to primitive '" + actor->getName() + "'");
        
        // Setup physics component
        auto physicsComponent = actor->getComponent<PhysicsComponent>();
        setupPhysicsComponent(physicsComponent.get(), config, type);
    }
    
    // All objects get mesh and material setup if they have those components
    auto meshComponent = actor->getComponent<MeshComponent>();
    auto materialComponent = actor->getComponent<MaterialComponent>();
    
    if (meshComponent && config.needsMesh) {
        setupMeshComponent(meshComponent.get(), type);
    }
    
    if (materialComponent && config.needsMaterial) {
        setupMaterialComponent(materialComponent.get(), config);
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
            
        case PrimitiveType::Platform:
            config.needsMesh = true;
            config.needsPhysics = true;
            config.needsMaterial = true;
            config.physicsType = physics::dynamics::RigidBodyType::STATIC;
            config.mass = 0.0f;
            config.friction = 0.8f;
            config.restitution = 0.2f;
            config.materialColor = glm::vec3(0.4f, 0.6f, 0.4f); // Green platform color
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
            config.needsMesh = false;     // Lights are invisible
            config.needsMaterial = false; // No material needed for lights
            config.needsPhysics = false;  // Lights don't need physics
            config.lightType = LightType::Point;
            config.intensity = 1.0f;
            config.lightColor = glm::vec3(1.0f, 1.0f, 1.0f);
            break;
            
        case PrimitiveType::DirectionalLight:
            config.needsLight = true;
            config.needsMesh = false;     // Lights are invisible
            config.needsMaterial = false; // No material needed for lights
            config.needsPhysics = false;  // Lights don't need physics
            config.lightType = LightType::Directional;
            config.intensity = 3.0f;
            config.lightColor = glm::vec3(1.0f, 1.0f, 0.9f);
            break;
            
        case PrimitiveType::SpotLight:
            config.needsLight = true;
            config.needsMesh = false;     // Lights are invisible
            config.needsMaterial = false; // No material needed for lights
            config.needsPhysics = false;  // Lights don't need physics
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
            
        case PrimitiveType::Platform:
            // Use thick platform box - perfect for floors, bridges, ledges
            physics->createBoxShape(glm::vec3(2.0f, 0.2f, 2.0f)); // width=4, height=0.4, depth=4
            break;
            
        case PrimitiveType::Cylinder:
            // Use proper cylinder shape instead of box approximation
            physics->createCylinderShape(0.5f, 1.0f); // radius=0.5, height=1.0
            break;
            
        case PrimitiveType::Cone:
            // For cone, use a capsule as approximation (better than box)
            physics->createCapsuleShape(0.5f, 1.0f); // radius=0.5, height=1.0
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
        case PrimitiveType::Platform:
            return generatePlatformMesh();
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

std::shared_ptr<Model> ComponentFactory::generatePlatformMesh(float width, float height, float depth) {
    auto model = std::make_shared<Model>();
    
    // Generate a thick platform (essentially a flattened box)
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    float halfWidth = width * 0.5f;
    float halfHeight = height * 0.5f;
    float halfDepth = depth * 0.5f;

    // Define the 8 corners of the platform box
    glm::vec3 corners[8] = {
        {-halfWidth, -halfHeight, -halfDepth}, // 0: bottom-left-back
        { halfWidth, -halfHeight, -halfDepth}, // 1: bottom-right-back
        { halfWidth, -halfHeight,  halfDepth}, // 2: bottom-right-front
        {-halfWidth, -halfHeight,  halfDepth}, // 3: bottom-left-front
        {-halfWidth,  halfHeight, -halfDepth}, // 4: top-left-back
        { halfWidth,  halfHeight, -halfDepth}, // 5: top-right-back
        { halfWidth,  halfHeight,  halfDepth}, // 6: top-right-front
        {-halfWidth,  halfHeight,  halfDepth}  // 7: top-left-front
    };

    // Face normals
    glm::vec3 normals[6] = {
        { 0.0f,  1.0f,  0.0f}, // top
        { 0.0f, -1.0f,  0.0f}, // bottom
        { 0.0f,  0.0f,  1.0f}, // front
        { 0.0f,  0.0f, -1.0f}, // back
        { 1.0f,  0.0f,  0.0f}, // right
        {-1.0f,  0.0f,  0.0f}  // left
    };

    // Define faces (each face has 4 vertices)
    int faceVertices[6][4] = {
        {7, 6, 5, 4}, // top face
        {0, 1, 2, 3}, // bottom face
        {3, 2, 6, 7}, // front face
        {4, 5, 1, 0}, // back face
        {2, 1, 5, 6}, // right face
        {0, 3, 7, 4}  // left face
    };

    // UV coordinates for each face
    glm::vec2 faceUVs[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
    };

    // Generate vertices for each face
    for (int face = 0; face < 6; ++face) {
        for (int vert = 0; vert < 4; ++vert) {
            Vertex vertex;
            vertex.position = corners[faceVertices[face][vert]];
            vertex.normal = normals[face];
            vertex.color = glm::vec3(1.0f); // White color
            vertex.texCoord = faceUVs[vert];
            vertices.push_back(vertex);
        }
    }

    // Generate indices (2 triangles per face)
    for (int face = 0; face < 6; ++face) {
        int baseIndex = face * 4;
        
        // First triangle
        indices.push_back(baseIndex + 0);
        indices.push_back(baseIndex + 1);
        indices.push_back(baseIndex + 2);
        
        // Second triangle
        indices.push_back(baseIndex + 2);
        indices.push_back(baseIndex + 3);
        indices.push_back(baseIndex + 0);
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

void ComponentFactory::setupPhysicsShapeFromMesh(PhysicsComponent* physics, MeshComponent* mesh) {
    if (!physics || !mesh) {
        OHAO_LOG_WARNING("Cannot setup physics shape: physics or mesh component is null");
        return;
    }
    
    // Get the model from mesh component
    auto model = mesh->getModel();
    if (!model) {
        OHAO_LOG_WARNING("Cannot setup physics shape: mesh component has no model");
        return;
    }
    
    // Create collision shape from the model's geometry
    physics->createCollisionShapeFromModel(*model);
    
    OHAO_LOG("Created collision shape from mesh with " + 
             std::to_string(model->vertices.size()) + " vertices");
}

} // namespace ohao