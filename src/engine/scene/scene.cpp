#include "scene.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/component/component_factory.hpp"
#include "engine/component/component_pack.hpp"
#include "engine/actor/actor.hpp"
#include "engine/asset/model.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/world/physics_settings.hpp"
#include "engine/serialization/map_io.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/components/console_widget.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace ohao {

// Define file extension
const std::string Scene::FILE_EXTENSION = ".ohscene";

Scene::Scene(const std::string& name)
    : name(name)
    , needsBufferUpdate(false)
{
    // Create a root actor for internal hierarchy grouping
    rootNode = std::make_shared<Actor>("World");
    registerActor(rootNode);
    
    // Initialize physics world
    physicsWorld = std::make_unique<physics::PhysicsWorld>();
    physics::PhysicsSettings settings;
    settings.gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    physicsWorld->initialize(settings);
    
    std::cout << "Scene '" << name << "' created with physics world" << std::endl;
}

Scene::~Scene() {
    destroy();
}

Actor::Ptr Scene::createActor(const std::string& name) {
    Actor::Ptr actor = std::make_shared<Actor>(name);
    addActor(actor);
    return actor;
}

Actor::Ptr Scene::createActorWithComponents(const std::string& name, PrimitiveType primitiveType) {
    // Use ComponentFactory to create actor with appropriate components
    auto actor = ComponentFactory::createActorWithComponents(this, name, primitiveType);
    
    if (actor) {
        OHAO_LOG("Created actor '" + name + "' with components for primitive type: " + 
                 std::to_string(static_cast<int>(primitiveType)));
    } else {
        OHAO_LOG_ERROR("Failed to create actor '" + name + "' with components");
    }
    
    return actor;
}

void Scene::addActor(Actor::Ptr actor) {
    if (!actor) return;
    
    // Check if actor is already in this scene
    auto it = actors.find(actor->getID());
    if (it != actors.end()) return;
    
    // Register actor with this scene
    registerActorHierarchy(actor);
}

void Scene::removeActor(Actor::Ptr actor) {
    if (!actor) return;
    
    // Unregister from scene
    unregisterActorHierarchy(actor);
}

void Scene::removeActor(const std::string& name) {
    auto it = actorsByName.find(name);
    if (it != actorsByName.end()) {
        removeActor(it->second);
    }
}

void Scene::removeActor(uint64_t id) {
    auto it = actors.find(id);
    if (it != actors.end()) {
        removeActor(it->second);
    }
}

void Scene::removeAllActors() {
    // Make a copy of the actors to avoid modification during iteration
    auto actorsCopy = actors;
    
    // Remove each actor
    for (auto& [id, actor] : actorsCopy) {
        removeActor(actor);
    }
    
    // Clear component lists
    meshComponents.clear();
    physicsComponents.clear();
    
    // Ensure maps are empty
    actors.clear();
    actorsByName.clear();
}

Actor::Ptr Scene::findActor(const std::string& name) const {
    auto it = actorsByName.find(name);
    if (it != actorsByName.end()) {
        return it->second;
    }
    return nullptr;
}

Actor::Ptr Scene::findActor(uint64_t id) const {
    auto it = actors.find(id);
    if (it != actors.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<Actor::Ptr> Scene::findActorsByName(const std::string& partialName) const {
    std::vector<Actor::Ptr> result;
    
    for (const auto& [name, actor] : actorsByName) {
        if (name.find(partialName) != std::string::npos) {
            result.push_back(actor);
        }
    }
    
    return result;
}

std::vector<Actor::Ptr> Scene::findActorsByTag(const std::string& tag) const {
    // TODO: Implement tag system for actors
    return {};
}

const std::unordered_map<uint64_t, Actor::Ptr>& Scene::getAllActors() const {
    return actors;
}

void Scene::onMeshComponentAdded(MeshComponent* component) {
    if (!component) return;
    
    // Add to mesh components list if not already there
    if (std::find(meshComponents.begin(), meshComponents.end(), component) == meshComponents.end()) {
        meshComponents.push_back(component);
        needsBufferUpdate = true;
    }
}

void Scene::onMeshComponentRemoved(MeshComponent* component) {
    if (!component) return;
    
    // Remove from mesh components list
    auto it = std::find(meshComponents.begin(), meshComponents.end(), component);
    if (it != meshComponents.end()) {
        meshComponents.erase(it);
        needsBufferUpdate = true;
    }
}

void Scene::onMeshComponentChanged(MeshComponent* component) {
    needsBufferUpdate = true;
}

void Scene::onPhysicsComponentAdded(PhysicsComponent* component) {
    if (!component) return;
    
    // Add to physics components list if not already there
    if (std::find(physicsComponents.begin(), physicsComponents.end(), component) == physicsComponents.end()) {
        physicsComponents.push_back(component);
    }
    
    // Connect the component to our physics world
    component->setPhysicsWorld(physicsWorld.get());
    
    // Initialize the component to ensure it creates a rigid body
    component->initialize();
    
    std::cout << "Physics component added to scene (total: " << physicsComponents.size() << ", rigid bodies: " << physicsWorld->getRigidBodyCount() << ")" << std::endl;
}

void Scene::onPhysicsComponentRemoved(PhysicsComponent* component) {
    if (!component) return;
    
    // Remove from physics components list
    auto it = std::find(physicsComponents.begin(), physicsComponents.end(), component);
    if (it != physicsComponents.end()) {
        physicsComponents.erase(it);
        std::cout << "Physics component removed from scene (remaining: " << physicsComponents.size() << ")" << std::endl;
    }
    
    // Disconnect from physics world
    component->setPhysicsWorld(nullptr);
}

void Scene::updatePhysics(float deltaTime) {
    if (physicsWorld) {
        physicsWorld->stepSimulation(deltaTime);
        
        // Update all physics components
        for (auto* component : physicsComponents) {
            if (component) {
                component->update(deltaTime);
            }
        }
    }
}

const std::string& Scene::getName() const {
    return name;
}

void Scene::setName(const std::string& newName) {
    name = newName;
}


void Scene::initialize() {
    // Initialize all actors
    for (auto& [id, actor] : actors) {
        actor->initialize();
    }
}

void Scene::update(float deltaTime) {
    // Update all actors
    for (auto& [id, actor] : actors) {
        if (actor->isActive()) {
            actor->update(deltaTime);
        }
    }
}

void Scene::render() {
    // Rendering happens in the renderer, not here
    // This is just a placeholder for potential scene-level rendering logic
}

void Scene::destroy() {
    removeAllActors();
}

bool Scene::importModel(const std::string& filename, Actor::Ptr targetActor) {
    // Check if file exists
    if (!std::filesystem::exists(filename)) {
        std::cerr << "File not found: " << filename << std::endl;
        return false;
    }
    
    // Create a model object
    auto model = std::make_shared<Model>();
    if (!model->loadFromOBJ(filename)) {
        std::cerr << "Failed to load model: " << filename << std::endl;
        return false;
    }
    
    // Generate a name from the filename for later use
    std::string baseName = std::filesystem::path(filename).filename().stem().string();
    
    // If no target actor was provided, create a new one
    if (!targetActor) {
        targetActor = createActor(baseName);
    }
    
    // STANDARDIZED COMPONENT SETUP using Component Packs
    // This ensures all imported models get the same components as primitives
    
    // Apply standard component pack (mesh + material + physics)
    StandardObjectPack::applyTo(targetActor);
    
    std::cout << "Applied StandardObjectPack to '" << targetActor->getName() 
              << "' (" << StandardObjectPack::count() << " components)" << std::endl;
    
    // Get the components (they're guaranteed to exist now)
    auto meshComponent = targetActor->getComponent<MeshComponent>();
    auto materialComponent = targetActor->getComponent<MaterialComponent>();
    auto physicsComponent = targetActor->getComponent<PhysicsComponent>();
    
    // Set the model
    meshComponent->setModel(model);
    
    // Configure physics properties based on model type
    // TODO: Add import option to choose static vs dynamic
    bool shouldBeStatic = (baseName.find("room") != std::string::npos || 
                          baseName.find("cornell") != std::string::npos ||
                          baseName.find("building") != std::string::npos ||
                          baseName.find("wall") != std::string::npos);
    
    if (shouldBeStatic) {
        physicsComponent->setRigidBodyType(physics::dynamics::RigidBodyType::STATIC);
        physicsComponent->setMass(0.0f);
        physicsComponent->setFriction(0.8f);
        physicsComponent->setRestitution(0.2f);
        std::cout << "Set imported model '" << targetActor->getName() << "' as STATIC (room/building detected)" << std::endl;
    } else {
        physicsComponent->setRigidBodyType(physics::dynamics::RigidBodyType::DYNAMIC);
        physicsComponent->setMass(1.0f);
        physicsComponent->setRestitution(0.3f);
        physicsComponent->setFriction(0.5f);
        std::cout << "Set imported model '" << targetActor->getName() << "' as DYNAMIC" << std::endl;
    }
    
    // Create appropriate collision shape based on model bounds
    // For now, use a bounding box - could be improved with mesh collision later
    auto transform = targetActor->getTransform();
    if (transform) {
        glm::vec3 scale = transform->getScale();
        physicsComponent->createBoxShape(scale * 0.5f);
    } else {
        // Default box shape if no transform
        physicsComponent->createBoxShape(1.0f, 1.0f, 1.0f);
    }
    
    // Connect to physics world
    physicsComponent->setTransformComponent(targetActor->getTransform());
    
    std::cout << "Added PhysicsComponent to imported model '" << targetActor->getName() 
              << "' - Collision shape configured" << std::endl;
    
    // Convert MTL materials to PBR materials
    if (!model->materials.empty()) {
        // For now, use the first material found in the MTL
        // TODO: Handle multi-material objects properly
        const auto& mtlMaterial = model->materials.begin()->second;
        
        Material pbrMaterial;
        pbrMaterial.name = mtlMaterial.name;
        
        // Convert MTL properties to PBR properties
        pbrMaterial.baseColor = mtlMaterial.diffuse;
        pbrMaterial.emissive = mtlMaterial.emission;
        
        // Estimate PBR properties from MTL properties
        // MTL shininess to PBR roughness conversion (rough approximation)
        pbrMaterial.roughness = glm::clamp(1.0f - (mtlMaterial.shininess / 128.0f), 0.0f, 1.0f);
        
        // Use specular intensity to estimate metallic property
        float specularIntensity = glm::length(mtlMaterial.specular);
        pbrMaterial.metallic = specularIntensity > 0.8f ? 0.8f : 0.0f; // Conservative metallic estimation
        
        // Set opacity
        pbrMaterial.ao = mtlMaterial.opacity;
        
        // Convert texture paths (make them relative to the model file directory)
        std::string modelDirectory = std::filesystem::path(filename).parent_path().string();
        if (!modelDirectory.empty() && modelDirectory.back() != '/' && modelDirectory.back() != '\\') {
            modelDirectory += "/";
        }
        
        if (!mtlMaterial.diffuseTexture.empty()) {
            pbrMaterial.albedoTexture = modelDirectory + mtlMaterial.diffuseTexture;
            pbrMaterial.useAlbedoTexture = true;
            std::cout << "  -> Albedo texture: " << pbrMaterial.albedoTexture << std::endl;
        }
        if (!mtlMaterial.normalTexture.empty()) {
            pbrMaterial.normalTexture = modelDirectory + mtlMaterial.normalTexture;
            pbrMaterial.useNormalTexture = true;
            std::cout << "  -> Normal texture: " << pbrMaterial.normalTexture << std::endl;
        }
        if (!mtlMaterial.specularTexture.empty()) {
            pbrMaterial.metallicTexture = modelDirectory + mtlMaterial.specularTexture;
            pbrMaterial.useMetallicTexture = true;
            std::cout << "  -> Metallic texture: " << pbrMaterial.metallicTexture << std::endl;
        }
        
        materialComponent->setMaterial(pbrMaterial);
        
        std::cout << "Applied material '" << pbrMaterial.name << "' to actor '" << targetActor->getName() 
                  << "' - baseColor(" << pbrMaterial.baseColor.x << "," << pbrMaterial.baseColor.y << "," << pbrMaterial.baseColor.z 
                  << "), roughness=" << pbrMaterial.roughness << ", metallic=" << pbrMaterial.metallic << std::endl;
    }
    
    return true;
}

bool Scene::saveToFile(const std::string& filename) {
    MapIO io(this);
    return io.save(filename);
}

bool Scene::loadFromFile(const std::string& filename) {
    MapIO io(this);
    return io.load(filename);
}

bool Scene::updateSceneBuffers() {
    // Mark as up-to-date
    needsBufferUpdate = false;
    
    // Call VulkanContext to update actual buffers in GPU memory
    if (auto* vulkanContext = VulkanContext::getContextInstance()) {
        return vulkanContext->updateSceneBuffers();
    }
    
    return false;
}

void Scene::registerActor(Actor::Ptr actor) {
    // Add to maps
    actors[actor->getID()] = actor;
    actorsByName[actor->getName()] = actor;
    
    // Set scene pointer
    actor->setScene(this);
}

void Scene::unregisterActor(Actor::Ptr actor) {
    // Remove from maps
    actors.erase(actor->getID());
    actorsByName.erase(actor->getName());
    
    // Clear scene pointer
    actor->setScene(nullptr);
}

void Scene::setupDefaultMaterial(Material& material) {
    // TODO: Set up default material properties
}

void Scene::registerActorHierarchy(Actor::Ptr actor) {
    // Register this actor
    registerActor(actor);
    
    // Get immediate children
    for (auto* childPtr : actor->getChildren()) {
        // Convert to shared_ptr (assuming the child is managed by shared_ptr)
        auto childIt = actors.find(childPtr->getID());
        if (childIt != actors.end()) {
            Actor::Ptr child = childIt->second;
            
            // Register this child (avoids duplicate registration)
            registerActorHierarchy(child);
        }
    }
}

void Scene::unregisterActorHierarchy(Actor::Ptr actor) {
    // Make a copy of children to avoid modification during iteration
    std::vector<Actor*> childrenCopy = actor->getChildren();
    
    // Get immediate children
    for (auto* childPtr : childrenCopy) {
        // Convert to shared_ptr
        auto childIt = actors.find(childPtr->getID());
        if (childIt != actors.end()) {
            Actor::Ptr child = childIt->second;
            
            // Unregister this child
            unregisterActorHierarchy(child);
        }
    }
    
    // Unregister this actor
    unregisterActor(actor);
}

void Scene::addPhysicsToAllObjects() {
    std::cout << "Adding physics components to all objects in scene..." << std::endl;
    
    int physicsObjectsCreated = 0;
    
    // Iterate through all actors in the scene
    for (const auto& [id, actor] : actors) {
        if (!actor) continue;
        
        // Skip actors that already have physics components
        if (actor->getComponent<PhysicsComponent>()) {
            std::cout << "Actor '" << actor->getName() << "' already has physics component" << std::endl;
            continue;
        }
        
        // Only add physics to actors that have mesh components (visual objects)
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (!meshComponent) {
            std::cout << "Skipping actor '" << actor->getName() << "' - no mesh component" << std::endl;
            continue;
        }
        
        // Add physics component
        auto physicsComponent = actor->addComponent<PhysicsComponent>();
        if (physicsComponent) {
            // Set up physics properties for a dynamic object
            physicsComponent->setRigidBodyType(physics::dynamics::RigidBodyType::DYNAMIC);
            physicsComponent->setMass(1.0f);  // 1 kg
            physicsComponent->setRestitution(0.3f);  // Some bounce
            physicsComponent->setFriction(0.5f);     // Medium friction
            
            // Create a box collision shape based on object scale
            auto transform = actor->getTransform();
            if (transform) {
                glm::vec3 scale = transform->getScale();
                // Create box collision shape (half-extents = scale / 2)
                physicsComponent->createBoxShape(scale * 0.5f);
            } else {
                // Default box shape if no transform
                physicsComponent->createBoxShape(0.5f, 0.5f, 0.5f);
            }
            
            // Set transform component reference for synchronization
            physicsComponent->setTransformComponent(actor->getTransform());
            
            std::cout << "Added physics to actor '" << actor->getName() << "' with box collision shape" << std::endl;
            physicsObjectsCreated++;
        }
    }
    
    std::cout << "Physics setup complete. Created " << physicsObjectsCreated << " physics objects." << std::endl;
    std::cout << "Total physics components in scene: " << physicsComponents.size() << std::endl;
    std::cout << "Total rigid bodies in physics world: " << physicsWorld->getRigidBodyCount() << std::endl;
}

} // namespace ohao