#include "scene.hpp"
#include "scene_node.hpp"
#include "../actor/actor.hpp"
#include "../component/mesh_component.hpp"
#include "../component/physics_component.hpp"
#include "../asset/model.hpp"
#include "../physics/collision_shape.hpp"
#include "../serialization/scene_serializer.hpp"
#include "../../renderer/vulkan_context.hpp"
#include "../../ui/components/console_widget.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <atomic>
#include <nlohmann/json.hpp>

namespace ohao {

// Define file extension
const std::string Scene::FILE_EXTENSION = ".ohscene";

Scene::Scene(const std::string& name)
    : name(name)
    , needsBufferUpdate(false)
    , dirty(false)
    , changeTracker(std::make_unique<SceneChangeTracker>(this))
{
    // Note: The constructor no longer creates the root node
    // Root node is created in setupRootNode() method
}

bool Scene::setupRootNode() {
    try {
        // We're switching to a non-hierarchical approach, so no root node is needed
        // Just return success without creating a root node
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing scene: " << e.what() << std::endl;
        return false;
    }
}

Scene::~Scene() {
    try {
        // Clear change tracker first
        if (changeTracker) {
            changeTracker->clearHistory();
            changeTracker.reset();
        }
        
        // Explicitly clear all actors before clearing maps
        // This is important to break circular references
        reset();
        
    } catch (const std::exception& e) {
        std::cerr << "Error in Scene destructor: " << e.what() << std::endl;
    }
}

void Scene::reset() {
    try {
        // First detach all actors from scene to prevent circular references
        for (auto& [id, actor] : actors) {
            if (actor) {
                actor->setScene(nullptr);
            }
        }
        
        // Clear component lists
        meshComponents.clear();
        physicsComponents.clear();
        
        // Break parent-child relationships - maintain flat structure
        for (auto& [id, actor] : actors) {
            if (actor) {
                // Safely detach from any parent
                actor->detachFromParent();
                
                // Clear children list
                auto children = actor->getChildren(); // Get copy of children
                for (auto* child : children) {
                    if (child) {
                        actor->removeChild(child);
                    }
                }
            }
        }
        
        // Clear maps
        actors.clear();
        actorsByName.clear();
        
        // Clear lights
        lights.clear();
        
    } catch (const std::exception& e) {
        std::cerr << "Error resetting scene: " << e.what() << std::endl;
    }
}

Actor::Ptr Scene::createActor(const std::string& name) {
    // Create actor with unique name
    Actor::Ptr actor = std::make_shared<Actor>(name);
    
    // Generate a unique ID if needed
    if (actor->getID() == 0) {
        static std::atomic<uint64_t> nextID = 100; // Start from 100
        actor->setID(nextID++);
    }
    
    // Add default transform component if needed
    if (!actor->getComponent<TransformComponent>()) {
        actor->addComponent<TransformComponent>();
    }
    
    // Add to scene
    addActor(actor);
    
    // Mark scene as needing update
    setDirty();
    
    return actor;
}

void Scene::addActor(Actor::Ptr actor) {
    if (!actor) return;
    
    // Store old state for change tracking
    trackActorAdded(actor);
    
    // Check if actor is already in this scene
    auto it = actors.find(actor->getID());
    if (it != actors.end()) return;
    
    // Register actor with this scene
    registerActorHierarchy(actor);
    setDirty();
}

void Scene::removeActor(Actor::Ptr actor) {
    if (!actor) return;
    
    // Store old state for change tracking
    trackActorRemoved(actor);
    
    // Unregister from scene
    unregisterActorHierarchy(actor);
    setDirty();
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
    // First, track this operation for undo/redo
    for (auto& [id, actor] : actors) {
        if (actor) {
            trackActorRemoved(actor);
        }
    }
    
    // Make a copy of the actors to avoid modification during iteration
    auto actorsCopy = actors;
    
    // Remove each actor
    for (auto& [id, actor] : actorsCopy) {
        if (actor) {
            try {
                removeActor(actor);
            } catch (const std::exception& e) {
                std::cerr << "Error removing actor: " << e.what() << std::endl;
            }
        }
    }
    
    // Mark the scene as dirty
    setDirty();
}

Actor::Ptr Scene::findActor(const std::string& name) const {
    auto it = actorsByName.find(name);
    if (it != actorsByName.end()) {
        return it->second;
    }
    return nullptr;
}

// Implementation of the getActor method
std::shared_ptr<Actor> Scene::getActor(const std::string& name) {
    return findActor(name);
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

// Implementation for the getActors method
const std::vector<std::shared_ptr<Actor>>& Scene::getActors() const {
    // We need to provide a persistent vector
    static std::vector<std::shared_ptr<Actor>> actorList;
    actorList.clear();
    
    // Fill from our map
    for (const auto& [id, actor] : actors) {
        actorList.push_back(actor);
    }
    
    return actorList;
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
    if (name != newName) {
    name = newName;
        setDirty();
    }
}

void Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
    setDirty();
}

void Scene::removeLight(const std::string& name) {
    lights.erase(name);
    setDirty();
}

void Scene::updateLight(const std::string& name, const Light& light) {
    lights[name] = light;
    setDirty();
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

void Scene::reinitialize(bool forceNewRoot) {
    if (forceNewRoot) {
        // Clear root node first to force recreation
        rootNode.reset();
    }
    
    // Call base initialize
    if (!setupRootNode()) {
        std::cerr << "Failed to reinitialize scene with forceNewRoot=" << forceNewRoot << std::endl;
    }
    
    // Initialize all actors
    initialize();
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

bool Scene::saveToFile(const std::string& filePath) {
    try {
        // Create a serializer
        SceneSerializer serializer(this);
        
        // Serialize to file
        if (!serializer.serialize(filePath)) {
            std::cerr << "Failed to save scene to: " << filePath << std::endl;
            return false;
        }
        
        // Update dirty flag
        dirty = false;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception while saving scene: " << e.what() << std::endl;
        return false;
    }
}

bool Scene::loadFromFile(const std::string& filePath, Scene::Ptr& outScene) {
    try {
        // Get scene name from file path (without extension)
        std::string name = std::filesystem::path(filePath).stem().string();
        
        // Create new scene
        auto scene = Scene::create(name);
        if (!scene) {
            std::cerr << "Failed to create scene for loading: " << filePath << std::endl;
            return false;
        }
        
        // Set project path
        scene->setProjectPath(std::filesystem::path(filePath).parent_path().parent_path().string());
        
        // Load the scene
        SceneSerializer serializer(scene.get());
        if (!serializer.deserialize(filePath)) {
            std::cerr << "Failed to load scene from file: " << filePath << std::endl;
            return false;
        }
        
        // Set the output parameter
        outScene = scene;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception while loading scene: " << e.what() << std::endl;
        return false;
    }
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
    
    // Check for mesh components and register them
    for (const auto& component : actor->getAllComponents()) {
        auto meshComponent = std::dynamic_pointer_cast<MeshComponent>(component);
        if (meshComponent) {
            onMeshComponentAdded(meshComponent.get());
        }
        
        auto physicsComponent = std::dynamic_pointer_cast<PhysicsComponent>(component);
        if (physicsComponent) {
            onPhysicsComponentAdded(physicsComponent.get());
        }
    }
    
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

void Scene::setDirty(bool state) {
    dirty = state;
    
    // Propagate to VulkanContext if available
    if (auto context = VulkanContext::getContextInstance()) {
        if (state) {
            context->markSceneModified();
        } else {
            context->clearSceneModified();
        }
    }
}

void Scene::clearDirty() {
    dirty = false;
    
    // Propagate to VulkanContext if available
    if (auto context = VulkanContext::getContextInstance()) {
        context->clearSceneModified();
    }
}

// Change tracking methods
void Scene::trackActorAdded(std::shared_ptr<Actor> actor) {
    if (!actor) return;
    changeTracker->addChange(std::make_unique<ActorAddedChange>(this, actor));
}

void Scene::trackActorRemoved(std::shared_ptr<Actor> actor) {
    if (!actor) return;
    changeTracker->addChange(std::make_unique<ActorRemovedChange>(this, actor));
}

void Scene::trackComponentModified(Component* component, const nlohmann::json& oldState, const nlohmann::json& newState) {
    if (!component) return;
    changeTracker->addChange(std::make_unique<ComponentModifiedChange>(this, component, oldState, newState));
}

void Scene::trackActorModified(Actor* actor, const nlohmann::json& oldState, const nlohmann::json& newState) {
    if (!actor) return;
    changeTracker->addChange(std::make_unique<ActorModifiedChange>(this, actor, oldState, newState));
}

bool Scene::canUndo() const {
    return changeTracker->canUndo();
}

bool Scene::canRedo() const {
    return changeTracker->canRedo();
}

void Scene::undo() {
    changeTracker->undo();
}

void Scene::redo() {
    changeTracker->redo();
}

void Scene::clearHistory() {
    changeTracker->clearHistory();
}

void Scene::saveHistory(const std::string& filename) const {
    changeTracker->saveHistory(filename);
}

void Scene::loadHistory(const std::string& filename) {
    changeTracker->loadHistory(filename);
}

std::string Scene::getLastChangeDescription() const {
    return changeTracker->getLastChangeDescription();
}

std::vector<std::string> Scene::getChangeHistory() const {
    return changeTracker->getChangeHistory();
}

// Serialization
nlohmann::json Scene::serialize() const {
    nlohmann::json data;
    
    // Basic scene properties
    data["name"] = name;
    data["version"] = "1.0";
    
    // Serialize descriptor
    data["descriptor"] = {
        {"name", descriptor.name},
        {"version", descriptor.version},
        {"createdBy", descriptor.createdBy},
        {"lastModified", descriptor.lastModified}
    };
    
    // Serialize tags
    data["descriptor"]["tags"] = descriptor.tags;
    
    // Serialize metadata
    data["descriptor"]["metadata"] = descriptor.metadata;
    
    // Serialize actors
    data["actors"] = nlohmann::json::array();
    for (const auto& [id, actor] : actors) {
        data["actors"].push_back(actor->serialize());
    }
    
    // Serialize lights
    data["lights"] = nlohmann::json::array();
    for (const auto& [name, light] : lights) {
        nlohmann::json lightData;
        lightData["name"] = name;
        lightData["position"] = {light.position.x, light.position.y, light.position.z};
        lightData["color"] = {light.color.x, light.color.y, light.color.z};
        lightData["intensity"] = light.intensity;
        lightData["enabled"] = light.enabled;
        data["lights"].push_back(lightData);
    }
    
    return data;
}

void Scene::deserialize(const nlohmann::json& data) {
    // Clear existing scene data
    removeAllActors();
    lights.clear();
    
    // Basic scene properties
    if (data.contains("name")) {
        name = data["name"];
    }
    
    // Deserialize descriptor
    if (data.contains("descriptor")) {
        auto& desc = data["descriptor"];
        
        if (desc.contains("name")) descriptor.name = desc["name"];
        if (desc.contains("version")) descriptor.version = desc["version"];
        if (desc.contains("createdBy")) descriptor.createdBy = desc["createdBy"];
        if (desc.contains("lastModified")) descriptor.lastModified = desc["lastModified"];
        
        if (desc.contains("tags")) descriptor.tags = desc["tags"].get<std::vector<std::string>>();
        if (desc.contains("metadata")) descriptor.metadata = desc["metadata"].get<std::unordered_map<std::string, std::string>>();
    }
    
    // Deserialize actors
    if (data.contains("actors") && data["actors"].is_array()) {
        for (const auto& actorData : data["actors"]) {
            auto actor = std::make_shared<Actor>();
            actor->deserialize(actorData);
            addActor(actor);
        }
    }
    
    // Deserialize lights
    if (data.contains("lights") && data["lights"].is_array()) {
        for (const auto& lightData : data["lights"]) {
            Light light;
            
            if (lightData.contains("position")) {
                const auto& pos = lightData["position"];
                light.position = glm::vec3(pos[0], pos[1], pos[2]);
            }
            
            if (lightData.contains("color")) {
                const auto& col = lightData["color"];
                light.color = glm::vec3(col[0], col[1], col[2]);
            }
            
            if (lightData.contains("intensity")) {
                light.intensity = lightData["intensity"];
            }
            
            if (lightData.contains("enabled")) {
                light.enabled = lightData["enabled"];
            }
            
            if (lightData.contains("name")) {
                lights[lightData["name"]] = light;
            }
        }
    }
    
    // Re-initialize the scene
    setupRootNode();
    
    // Mark as clean after loading
    clearDirty();
}

void Scene::beginModification() {
    // Mark the scene as dirty when starting modification
    setDirty();
    // The SceneChangeTracker doesn't have beginCommand(), we're just marking as dirty here
}

void Scene::endModification() {
    // Mark the scene as dirty when ending modification
    setDirty();
    // The SceneChangeTracker doesn't have endCommand(), changes are tracked via specific methods
}

bool Scene::isDirty() const {
    return dirty;
}

std::shared_ptr<Actor> Scene::getRootNode() const {
    // In a non-hierarchical approach, there is no root node
    // Return nullptr to indicate this
    return nullptr;
}

bool Scene::hasValidRoot() const {
    // In a non-hierarchical approach, we don't need a root node
    // Always return true since this is the expected state
    return true;
}

std::string Scene::getProjectDirPath() const {
    if (projectPath.empty()) {
        return "";
    }
    
    // Check if projectPath is a file or directory
    std::filesystem::path path(projectPath);
    if (std::filesystem::is_regular_file(path)) {
        return path.parent_path().string();
    }
    
    return projectPath;
}

// Initialize all scene actors
void Scene::initialize() {
    // Initialize all actors
    for (auto& [id, actor] : actors) {
        actor->initialize();
    }
}

} // namespace ohao