#include "scene.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/component_factory.hpp"
#include "scene/component/component_pack.hpp"
#include "scene/actor/actor.hpp"
#include "scene/asset/model.hpp"
#include "core/console_widget.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace ohao {

Scene::Scene(std::string_view name)
    : name(name)
    , needsBufferUpdate(false)
{
    // Create a root actor for internal hierarchy grouping
    rootNode = std::make_shared<Actor>("World");
    registerActor(rootNode);
    
    std::cout << "Scene '" << name << "' created" << std::endl;
}

Scene::~Scene() {
    destroy();
}

Actor::Ptr Scene::createActor(std::string_view name) {
    Actor::Ptr actor = std::make_shared<Actor>(name);
    addActor(actor);
    return actor;
}

Actor::Ptr Scene::createActorWithComponents(std::string_view name, PrimitiveType primitiveType) {
    // Use ComponentFactory to create actor with appropriate components
    auto actor = ComponentFactory::createActorWithComponents(this, name, primitiveType);
    
    if (actor) {
        OHAO_LOG("Created actor '" + std::string(name) + "' with components for primitive type: " + 
                 std::to_string(static_cast<int>(primitiveType)));
    } else {
        OHAO_LOG_ERROR("Failed to create actor '" + std::string(name) + "' with components");
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

void Scene::removeActor(std::string_view name) {
    auto it = actorsByName.find(std::string(name));
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
    
    // Ensure maps are empty
    actors.clear();
    actorsByName.clear();
}

Actor::Ptr Scene::findActor(std::string_view name) const {
    auto it = actorsByName.find(std::string(name));
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

std::vector<Actor::Ptr> Scene::findActorsByName(std::string_view partialName) const {
    std::vector<Actor::Ptr> result;
    result.reserve(actorsByName.size());
    for (const auto& [actorName, actor] : actorsByName) {
        if (actor && actorName.find(partialName) != std::string::npos) {
            result.push_back(actor);
        }
    }
    return result;
}

std::vector<Actor::Ptr> Scene::findActorsByTag(std::string_view tag) const {
    std::vector<Actor::Ptr> result;
    if (tag.empty()) return result;
    result.reserve(actors.size());
    for (const auto& [id, actor] : actors) {
        if (actor && actor->hasTag(tag)) {
            result.push_back(actor);
        }
    }
    return result;
}

void Scene::onMeshComponentAdded(MeshComponent* component) {
    if (!component) return;
    if (std::find(meshComponents.begin(), meshComponents.end(), component) == meshComponents.end()) {
        meshComponents.push_back(component);
        needsBufferUpdate = true;
    }
}

void Scene::onMeshComponentRemoved(MeshComponent* component) {
    if (!component) return;
    const auto before = meshComponents.size();
    std::erase(meshComponents, component);
    if (meshComponents.size() != before) {
        needsBufferUpdate = true;
    }
}

void Scene::onMeshComponentChanged(MeshComponent* component) {
    needsBufferUpdate = true;
}

const std::string& Scene::getName() const noexcept {
    return name;
}

void Scene::setName(std::string_view newName) {
    name = std::string(newName);
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

bool Scene::importModel(std::string_view filename, Actor::Ptr targetActor) {
    // Check if file exists
    if (!std::filesystem::exists(filename)) {
        std::cerr << "File not found: " << filename << std::endl;
        return false;
    }
    
    // Create a model object
    auto model = std::make_shared<Model>();
    if (!model->loadFromOBJ(std::string(filename))) {
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
    
    // Set the model
    meshComponent->setModel(model);
    
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

bool Scene::updateSceneBuffers() {
    // Mark as up-to-date
    needsBufferUpdate = false;

    // GPU buffer updates are handled by VulkanRenderer::updateSceneBuffers()
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

} // namespace ohao