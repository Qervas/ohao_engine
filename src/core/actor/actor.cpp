#include "actor.hpp"
#include "../component/component.hpp"
#include "../component/transform_component.hpp"
#include "../component/mesh_component.hpp"
#include "../component/physics_component.hpp"
#include "../scene/scene.hpp"
#include "../asset/model.hpp"
#include "../material/material.hpp"
#include <algorithm>
#include <iostream>

namespace ohao {

Actor::Actor(const std::string& name)
    : SceneObject(name)
    , scene(nullptr)
    , parent(nullptr)
    , active(true)
{
    // Add transform component by default
    addComponent<TransformComponent>();
}

Actor::~Actor() {
    // Remove all children
    while (!children.empty()) {
        removeChild(children[0]);
    }
    
    // Detach from parent if we have one
    if (parent) {
        detachFromParent();
    }
    
    // Clear all components
    removeAllComponents();
    
    // Ensure we're not in a scene
    if (scene) {
        scene->removeActor(std::dynamic_pointer_cast<Actor>(shared_from_this()));
    }
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
    if (parent == newParent) return;
    
    // Remove from old parent
    if (parent) {
        parent->removeChild(this);
    }
    
    parent = newParent;
    
    // Add to new parent
    if (parent) {
        parent->addChild(this);
    }
    
    // Update transform parent-child relationship
    auto transform = getTransform();
    if (transform) {
        if (parent) {
            transform->setParent(parent->getTransform());
        } else {
            transform->setParent(nullptr);
        }
    }
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
    if (parent) {
        parent->removeChild(this);
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
        }
        
        // Handle physics components
        if (auto physicsComp = std::dynamic_pointer_cast<PhysicsComponent>(component)) {
            scene->onPhysicsComponentAdded(physicsComp.get());
        }
        
        // Mark scene as dirty since components have changed
        scene->setDirty();
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

} // namespace ohao 