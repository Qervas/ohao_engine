#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include "engine/actor/actor.hpp"
#include "physics/world/physics_world.hpp"
#include "engine/component/component_factory.hpp"
#include <glm/glm.hpp>

namespace ohao {

class MeshComponent;
class TransformComponent;
class PhysicsComponent;
class Component;
class SceneNode;

// Scene descriptor for serialization
struct SceneDescriptor {
    std::string name;
    std::string version = "1.0";
    std::vector<std::string> tags;
    std::string createdBy;
    std::string lastModified;
    
    // Optional metadata
    std::unordered_map<std::string, std::string> metadata;
};

class Scene {
public:
    using Ptr = std::shared_ptr<Scene>;
    
    Scene(const std::string& name = "New Scene");
    ~Scene();
    
    // Actor management
    Actor::Ptr createActor(const std::string& name = "Actor");
    
    // NEW: ComponentFactory-based actor creation with automatic components
    Actor::Ptr createActorWithComponents(const std::string& name, PrimitiveType primitiveType);
    
    void addActor(Actor::Ptr actor);
    void removeActor(Actor::Ptr actor);
    void removeActor(const std::string& name);
    void removeActor(uint64_t id);
    void removeAllActors();
    
    // Actor lookup
    Actor::Ptr findActor(const std::string& name) const;
    Actor::Ptr findActor(uint64_t id) const;
    std::vector<Actor::Ptr> findActorsByName(const std::string& partialName) const;
    std::vector<Actor::Ptr> findActorsByTag(const std::string& tag) const;
    const std::unordered_map<uint64_t, Actor::Ptr>& getAllActors() const;
    
    // Legacy compatibility methods
    void addObject(const std::string& name, Actor::Ptr actor) { actorsByName[name] = actor; actors[actor->getID()] = actor; }
    void removeObject(const std::string& name) { removeActor(name); }
    Actor::Ptr getObjectByID(uint64_t id) { return findActor(id); }
    const std::unordered_map<std::string, Actor::Ptr>& getObjectsByName() const { return actorsByName; }
    
    // Component notifications
    void onMeshComponentAdded(MeshComponent* component);
    void onMeshComponentRemoved(MeshComponent* component);
    void onMeshComponentChanged(MeshComponent* component);
    
    void onPhysicsComponentAdded(PhysicsComponent* component);
    void onPhysicsComponentRemoved(PhysicsComponent* component);
    
    // Physics simulation
    void updatePhysics(float deltaTime);
    physics::PhysicsWorld* getPhysicsWorld() { return physicsWorld.get(); }
    const physics::PhysicsWorld* getPhysicsWorld() const { return physicsWorld.get(); }
    
    // Physics components access
    const std::vector<PhysicsComponent*>& getPhysicsComponents() const { return physicsComponents; }
    
    // Helper method to add physics to objects
    void addPhysicsToAllObjects();
    
    // Scene properties
    const std::string& getName() const;
    void setName(const std::string& name);
    
    // Scene lifecycle
    void initialize();
    void update(float deltaTime);
    void render();
    void destroy();
    
    // Model import
    bool importModel(const std::string& filename, Actor::Ptr targetActor = nullptr);
    
    // Serialization
    bool saveToFile(const std::string& filename);
    bool loadFromFile(const std::string& filename);
    
    // Root node accessor for backwards compatibility
    Actor::Ptr getRootNode() const { return rootNode; }
    
    // Buffer update
    bool updateSceneBuffers();
    bool hasBufferUpdateNeeded() const { return needsBufferUpdate; }
    
    // Scene descriptor methods
    const SceneDescriptor& getDescriptor() const { return descriptor; }
    void setDescriptor(const SceneDescriptor& desc) { descriptor = desc; }
    
    // Project path management 
    const std::string& getProjectPath() const { return projectPath; }
    void setProjectPath(const std::string& path) { projectPath = path; }
    
    // Scene file extension
    static const std::string FILE_EXTENSION;
    
private:
    std::string name;
    SceneDescriptor descriptor;
    
    // Actor tracking
    std::unordered_map<uint64_t, Actor::Ptr> actors;
    std::unordered_map<std::string, Actor::Ptr> actorsByName;
    
    // Component tracking
    std::vector<MeshComponent*> meshComponents;
    std::vector<PhysicsComponent*> physicsComponents;
    
    // Physics world
    std::unique_ptr<physics::PhysicsWorld> physicsWorld;
    
    // The root actor for the scene hierarchy
    Actor::Ptr rootNode;
    
    // Project path
    std::string projectPath;
    
    // Scene state
    bool needsBufferUpdate;
    
    // Helper methods
    void registerActor(Actor::Ptr actor);
    void unregisterActor(Actor::Ptr actor);
    void setupDefaultMaterial(class Material& material);
    
    // Hierarchy helpers
    void registerActorHierarchy(Actor::Ptr actor);
    void unregisterActorHierarchy(Actor::Ptr actor);
};

} // namespace ohao 