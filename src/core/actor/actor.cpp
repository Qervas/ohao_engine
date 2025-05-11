#include "actor.hpp"
#include "../component/component.hpp"
#include "../component/transform_component.hpp"
#include "../component/mesh_component.hpp"
#include "../component/physics_component.hpp"
#include "../scene/scene.hpp"
#include "../asset/model.hpp"
#include "../material/material.hpp"
#include "../../renderer/vulkan_context.hpp"
#include "../../ui/components/console_widget.hpp"
#include <algorithm>
#include <iostream>

namespace ohao {

Actor::Actor(const std::string& name)
    : name(name)
    , id(0)
    , scene(nullptr)
    , position(0.0f)
    , rotation(1.0f, 0.0f, 0.0f, 0.0f)
    , scale(1.0f)
    , modified(false)
    , parent(nullptr)
    , active(true)
{
    // Add transform component by default
    addComponent<TransformComponent>();
}

Actor::~Actor() {
    // First remove from parent if we have one
    // Do this before touching children to avoid circular references
    if (parent) {
        try {
            // Detach safely without using shared_from_this()
            parent->removeChild(this);
            parent = nullptr;
        } catch (const std::exception& e) {
            std::cerr << "Error detaching from parent during destruction: " << e.what() << std::endl;
            parent = nullptr;
        }
    }
    
    // Clear all children safely
    try {
        // Make a copy of children to avoid modification during iteration
        auto childrenCopy = children;
        children.clear(); // Clear the original list first
        
        // Now safely process the copy
        for (auto* child : childrenCopy) {
            if (child) {
                child->parent = nullptr; // Directly nullify parent pointer
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error clearing children during destruction: " << e.what() << std::endl;
    }
    
    // Clear all components
    removeAllComponents();
    
    // Don't call back to scene during destruction - can cause circular references
    scene = nullptr;
}

void Actor::setScene(Scene* newScene) {
    if (scene == newScene) return;
    
    // Leave old scene
    if (scene) {
        onRemovedFromScene();
    }
    
    scene = newScene;
    
    // Enter new scene
    if (scene) {
        onAddedToScene();
    }
}

void Actor::initialize() {
    // Initialize all components
    for (auto& component : components) {
        if (component->isEnabled()) {
            component->initialize();
        }
    }
    
    // Initialize all children
    for (auto* child : children) {
        child->initialize();
    }
}

void Actor::start() {
    // Start all components
    for (auto& component : components) {
        if (component->isEnabled()) {
            component->start();
        }
    }
    
    // Start all children
    for (auto* child : children) {
        child->start();
    }
}

void Actor::update(float deltaTime) {
    if (!active) return;
    
    // Update components
    for (auto& component : components) {
        if (component->isEnabled()) {
            component->update(deltaTime);
        }
    }
    
    // Update children
    for (auto* child : children) {
        child->update(deltaTime);
    }
}

void Actor::render() {
    if (!active) return;
    
    // Render components
    for (auto& component : components) {
        if (component->isEnabled()) {
            component->render();
        }
    }
    
    // Render children
    for (auto* child : children) {
        child->render();
    }
}

void Actor::destroy() {
    // Destroy all components
    for (auto& component : components) {
        component->destroy();
    }
    
    // Destroy all children
    for (auto* child : children) {
        child->destroy();
    }
}

void Actor::setParent(Actor* newParent) {
    // Don't do anything if the parent is the same
    if (parent == newParent) {
        return;
    }
    
    // Special case: check for null parent (detaching)
    if (newParent == nullptr) {
        detachFromParent();
        return;
    }
    
    // Safety check: don't allow parenting to self
    if (newParent == this) {
        std::cerr << "Error: Cannot set actor as its own parent" << std::endl;
        return;
    }
    
    // Check for circular references (if new parent is a child of this)
    Actor* checkNode = newParent;
    while (checkNode) {
        if (checkNode == this) {
            std::cerr << "Error: Cannot create circular parent references" << std::endl;
            return;
        }
        
        try {
            checkNode = checkNode->getParent();
        } catch (const std::exception& e) {
            // If an exception occurs during traversal, assume it's unsafe
            std::cerr << "Error checking for circular references: " << e.what() << std::endl;
            return;
        }
    }
    
    // First detach from current parent if any
    if (parent) {
        try {
            // Find this actor in parent's children
            auto it = std::find(parent->children.begin(), parent->children.end(), this);
            if (it != parent->children.end()) {
                parent->children.erase(it);
            }
        } catch (const std::exception& e) {
            // Log but continue
            std::cerr << "Error detaching from parent: " << e.what() << std::endl;
        }
    }
    
    // Update parent pointer
    parent = newParent;
    
    // Add to new parent's children if not already there
    try {
        if (parent) {
            // Ensure we're not already in the children list (avoid duplicates)
            auto it = std::find(parent->children.begin(), parent->children.end(), this);
            if (it == parent->children.end()) {
                parent->children.push_back(this);
            }
        }
    } catch (const std::exception& e) {
        // If we can't add to children, revert parent change
        std::cerr << "Error adding to parent's children: " << e.what() << std::endl;
        parent = nullptr;
        return;
    }
    
    // Update transform hierarchy
    updateWorldTransform();
}

void Actor::addChild(Actor* child) {
    if (!child || child == this) return;
    
    // Check if already a child
    if (std::find(children.begin(), children.end(), child) != children.end()) {
        return;
    }
    
    // Add to children
    children.push_back(child);
    
    // Update child's parent reference if needed
    if (child->getParent() != this) {
        child->setParent(this);
    }
}

void Actor::removeChild(Actor* child) {
    if (!child) return;
    
    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        children.erase(it);
        
        // Update child's parent reference if needed
        if (child->getParent() == this) {
            child->parent = nullptr;
        }
    }
}

void Actor::detachFromParent() {
    try {
        // If we have no parent, nothing to do
        if (!parent) return;
        
        // Verify parent is valid before operating on it
        try {
            const char* name = parent->getName().c_str();
            if (!name) {
                // If we can't even get the name, the pointer is likely corrupted
                throw std::runtime_error("Invalid parent pointer detected during detach");
            }
        } catch (const std::exception& e) {
            // Invalid parent, just null our pointer and return
            std::cerr << "Failed parent validation during detach: " << e.what() << std::endl;
            parent = nullptr;
            return;
        }
        
        // Safe removal from parent's children list
        try {
            auto it = std::find(parent->children.begin(), parent->children.end(), this);
            if (it != parent->children.end()) {
                parent->children.erase(it);
            }
        } catch (const std::exception& e) {
            // Log error but continue with detachment
            std::cerr << "Error removing from parent's children: " << e.what() << std::endl;
        }
        
        // Clear our parent pointer
        parent = nullptr;
        
        // Update transform
        updateWorldTransform();
        
    } catch (const std::exception& e) {
        // Ensure parent is null even if an exception occurs
        std::cerr << "Exception during detach from parent: " << e.what() << std::endl;
        parent = nullptr;
    }
}

TransformComponent* Actor::getTransform() const {
    return getComponent<TransformComponent>().get();
}

void Actor::removeAllComponents() {
    components.clear();
    componentsByType.clear();
}

void Actor::onComponentAdded(std::shared_ptr<Component> component) {
    // Notify scene of specific components like mesh/physics
    if (scene) {
        // Handle mesh components
        if (auto meshComp = std::dynamic_pointer_cast<MeshComponent>(component)) {
            scene->onMeshComponentAdded(meshComp.get());
            
            // Additional debug information
            OHAO_LOG_DEBUG("MeshComponent added to actor: " + getName());
            
            // Force a buffer update in VulkanContext if the mesh component has a model
            if (meshComp->getModel() && VulkanContext::getContextInstance()) {
                VulkanContext::getContextInstance()->updateSceneBuffers();
            }
        }
        
        // Handle physics components
        if (auto physicsComp = std::dynamic_pointer_cast<PhysicsComponent>(component)) {
            scene->onPhysicsComponentAdded(physicsComp.get());
        }
        
        // Mark scene as dirty since components have changed
        scene->setDirty();
    } else {
        OHAO_LOG_WARNING("Component added to actor with no scene: " + getName());
    }
}

void Actor::onComponentRemoved(std::shared_ptr<Component> component) {
    // Notify scene of specific components being removed
    if (scene) {
        // Handle mesh components
        if (auto meshComp = std::dynamic_pointer_cast<MeshComponent>(component)) {
            scene->onMeshComponentRemoved(meshComp.get());
        }
        
        // Handle physics components
        if (auto physicsComp = std::dynamic_pointer_cast<PhysicsComponent>(component)) {
            scene->onPhysicsComponentRemoved(physicsComp.get());
        }
        
        // Mark scene as dirty since components have changed
        scene->setDirty();
    }
}

void Actor::onAddedToScene() {
    // Register components with the scene
    for (auto& component : components) {
        if (auto meshComp = std::dynamic_pointer_cast<MeshComponent>(component)) {
            scene->onMeshComponentAdded(meshComp.get());
        }
        if (auto physicsComp = std::dynamic_pointer_cast<PhysicsComponent>(component)) {
            scene->onPhysicsComponentAdded(physicsComp.get());
        }
    }
    
    // Mark scene as dirty
    if (scene) {
        scene->setDirty();
    }
}

void Actor::onRemovedFromScene() {
    // Unregister components from the scene
    for (auto& component : components) {
        if (auto meshComp = std::dynamic_pointer_cast<MeshComponent>(component)) {
            scene->onMeshComponentRemoved(meshComp.get());
        }
        if (auto physicsComp = std::dynamic_pointer_cast<PhysicsComponent>(component)) {
            scene->onPhysicsComponentRemoved(physicsComp.get());
        }
    }
    
    // Mark scene as dirty
    if (scene) {
        scene->setDirty();
    }
}

// SceneObject compatibility implementations
void Actor::setModel(std::shared_ptr<Model> model) {
    // Get or create MeshComponent
    auto meshComponent = getComponent<MeshComponent>();
    if (!meshComponent) {
        meshComponent = addComponent<MeshComponent>();
    }
    
    // Set the model on the component
    meshComponent->setModel(model);
    
    // Mark scene as dirty
    if (scene) {
        scene->setDirty();
    }
}

std::shared_ptr<Model> Actor::getModel() const {
    auto meshComponent = getComponent<MeshComponent>();
    if (meshComponent) {
        return meshComponent->getModel();
    }
    return SceneObject::getModel();
}

void Actor::setMaterial(const Material& material) {
    auto meshComponent = getComponent<MeshComponent>();
    if (meshComponent) {
        meshComponent->setMaterial(material);
    }
    // Set in base class for compatibility
    SceneObject::setMaterial(material);
}

const Material& Actor::getMaterial() const {
    auto meshComponent = getComponent<MeshComponent>();
    if (meshComponent) {
        return meshComponent->getMaterial();
    }
    return SceneObject::getMaterial();
}

Material& Actor::getMaterial() {
    auto meshComponent = getComponent<MeshComponent>();
    if (meshComponent) {
        return meshComponent->getMaterial();
    }
    return SceneObject::getMaterial();
}

void Actor::beginModification() {
    if (!modified) {
        oldState = serialize();
        modified = true;
    }
}

void Actor::endModification() {
    if (modified) {
        auto scene = getScene();
        if (scene) {
            // Use the transform component for tracking actor modifications
            auto transform = getTransform();
            if (transform) {
                transform->beginModification();
                transform->endModification();
            }
        }
        modified = false;
    }
}

nlohmann::json Actor::serialize() const {
    nlohmann::json data;
    
    // Basic properties
    data["name"] = name;
    data["id"] = id;
    data["active"] = active;
    
    // Transform info
    data["position"] = {position.x, position.y, position.z};
    data["rotation"] = {rotation.x, rotation.y, rotation.z, rotation.w};
    data["scale"] = {scale.x, scale.y, scale.z};
    
    // Parent ID - use 0 for no parent
    data["parentId"] = parent ? parent->getID() : 0;
    
    // Serialize metadata
    if (!metadata.empty()) {
        data["metadata"] = metadata;
    }
    
    // Serialize components
    nlohmann::json componentsData = nlohmann::json::array();
    for (const auto& component : components) {
        if (component) {
            nlohmann::json componentData;
            componentData["type"] = component->getTypeName();
            componentData["data"] = component->serialize();
            componentData["enabled"] = component->isEnabled();
            componentsData.push_back(componentData);
        }
    }
    data["components"] = componentsData;
    
    return data;
}

void Actor::deserialize(const nlohmann::json& data) {
    if (data.contains("name")) {
        name = data["name"];
    }
    
    if (data.contains("id")) {
        id = data["id"];
    }
    
    if (data.contains("active")) {
        active = data["active"];
    }
    
    if (data.contains("position")) {
        const auto& pos = data["position"];
        position = glm::vec3(pos[0], pos[1], pos[2]);
    }
    
    if (data.contains("rotation")) {
        const auto& rot = data["rotation"];
        rotation = glm::quat(rot[3], rot[0], rot[1], rot[2]);
    }
    
    if (data.contains("scale")) {
        const auto& scl = data["scale"];
        scale = glm::vec3(scl[0], scl[1], scl[2]);
    }
    
    // Deserialize metadata
    if (data.contains("metadata") && data["metadata"].is_object()) {
        metadata = data["metadata"].get<std::unordered_map<std::string, std::string>>();
    }
    
    if (data.contains("components")) {
        // Clear existing components
        components.clear();
        componentsByType.clear();
        
        // Add new components
        for (const auto& componentData : data["components"]) {
            // TODO: Implement proper component type resolution and creation
            // This is a placeholder - you'll need to implement proper component deserialization
        }
    }
    
    // Update transform component with new position/rotation/scale
    auto transform = getTransform();
    if (transform) {
        transform->setPosition(position);
        transform->setRotation(rotation);
        transform->setScale(scale);
    }
    
    beginModification();
    endModification();
}

Actor* Actor::getParent() const {
    try {
        // Check if the parent pointer is null
        if (!parent) {
            return nullptr;
        }
        
        // Basic validation to ensure parent is a valid object
        const char* name = parent->getName().c_str();
        if (!name) {
            // If we can't even get the name, the pointer is likely corrupted
            throw std::runtime_error("Invalid parent pointer detected");
        }
        
        // Return the parent if it appears valid
        return parent;
    } catch (const std::exception& e) {
        // If any exception occurs, reset the parent pointer and return null
        std::cerr << "Error accessing parent: " << e.what() << std::endl;
        const_cast<Actor*>(this)->parent = nullptr;
        return nullptr;
    }
}

void Actor::updateWorldTransform() {
    try {
        // Try to get transform component
        auto transform = getComponent<TransformComponent>();
        if (!transform) {
            return; // No transform component to update
        }
        
        // Update transform parent-child relationship
        if (parent) {
            try {
                auto parentTransform = parent->getComponent<TransformComponent>();
                if (parentTransform) {
                    transform->setParent(parentTransform.get());
                }
            } catch (const std::exception& e) {
                std::cerr << "Error updating transform parent: " << e.what() << std::endl;
            }
        } else {
            // No parent, so no parent transform
            transform->setParent(nullptr);
        }
        
        // Force transform to update by accessing world matrix
        // This will internally calculate matrices if dirty
        transform->getWorldMatrix();
        
        // Recursively update children
        for (auto* child : children) {
            if (child) {
                try {
                    child->updateWorldTransform();
                } catch (const std::exception& e) {
                    std::cerr << "Error updating child transform: " << e.what() << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception in updateWorldTransform: " << e.what() << std::endl;
    }
}

// Add metadata methods
void Actor::setMetadata(const std::string& key, const std::string& value) {
    metadata[key] = value;
}

std::string Actor::getMetadata(const std::string& key) const {
    auto it = metadata.find(key);
    if (it != metadata.end()) {
        return it->second;
    }
    return "";
}

bool Actor::hasMetadata(const std::string& key) const {
    return metadata.find(key) != metadata.end();
}

} // namespace ohao 