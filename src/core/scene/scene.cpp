#include "scene.hpp"
#include "../component/mesh_component.hpp"
#include "../component/physics_component.hpp"
#include "../actor/actor.hpp"
#include "../asset/model.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace ohao {

Scene::Scene(const std::string& name)
    : name(name)
    , needsBufferUpdate(false)
{
    // Create a root node for backward compatibility
    rootNode = std::make_shared<Actor>("Root");
    registerActor(rootNode);
}

Scene::~Scene() {
    destroy();
}

Actor::Ptr Scene::createActor(const std::string& name) {
    Actor::Ptr actor = std::make_shared<Actor>(name);
    addActor(actor);
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
}

void Scene::onPhysicsComponentRemoved(PhysicsComponent* component) {
    if (!component) return;
    
    // Remove from physics components list
    auto it = std::find(physicsComponents.begin(), physicsComponents.end(), component);
    if (it != physicsComponents.end()) {
        physicsComponents.erase(it);
    }
}

const std::string& Scene::getName() const {
    return name;
}

void Scene::setName(const std::string& newName) {
    name = newName;
}

void Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

void Scene::removeLight(const std::string& name) {
    lights.erase(name);
}

void Scene::updateLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

Light* Scene::getLight(const std::string& name) {
    auto it = lights.find(name);
    if (it != lights.end()) {
        return &it->second;
    }
    return nullptr;
}

const std::unordered_map<std::string, Light>& Scene::getAllLights() const {
    return lights;
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
    lights.clear();
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
    
    // If no target actor was provided, create a new one
    if (!targetActor) {
        // Generate a name from the filename
        std::string baseName = std::filesystem::path(filename).filename().stem().string();
        targetActor = createActor(baseName);
    }
    
    // Add mesh component if it doesn't exist
    auto meshComponent = targetActor->getComponent<MeshComponent>();
    if (!meshComponent) {
        meshComponent = targetActor->addComponent<MeshComponent>();
    }
    
    // Set the model
    meshComponent->setModel(model);
    
    // Add physics component with default parameters
    if (!targetActor->hasComponent<PhysicsComponent>()) {
        auto physicsComponent = targetActor->addComponent<PhysicsComponent>();
        
        // Create a bounding box shape based on model dimensions
        // TODO: Calculate proper bounds from model
        physicsComponent->createBoxShape(glm::vec3(1.0f));
        physicsComponent->setStatic(true); // Static by default
    }
    
    return true;
}

bool Scene::saveToFile(const std::string& filename) {
    // TODO: Implement serialization
    return false;
}

bool Scene::loadFromFile(const std::string& filename) {
    // TODO: Implement deserialization
    return false;
}

bool Scene::updateSceneBuffers() {
    // This will need to interface with your Vulkan renderer
    // to rebuild the combined vertex/index buffers
    
    // Mark as up-to-date
    needsBufferUpdate = false;
    
    return true;
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
    // Get immediate children
    for (auto* childPtr : actor->getChildren()) {
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

} // namespace ohao