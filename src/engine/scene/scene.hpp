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
class SceneObject;

// Scene descriptor for serialization
struct SceneDescriptor {
    std::string name;
    std::string version = "1.0";
    std::vector<std::string> tags;
    std::string createdBy;
    std::string lastModified;
    
    std::unordered_map<std::string, std::string> metadata;
};

class Scene {
public:
    using Ptr = std::shared_ptr<Scene>;
    
    Scene(const std::string& name = "New Scene");
    ~Scene();
    
    // Actor management
    Actor::Ptr createActor(const std::string& name = "Actor");
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
    
    // Legacy compatibility (kept minimal)
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
    
    const std::vector<PhysicsComponent*>& getPhysicsComponents() const { return physicsComponents; }
    
    void addPhysicsToAllObjects();
    
    // Scene properties
    const std::string& getName() const;
    void setName(const std::string& name);
    
    // Scene lifecycle
    void initialize();
    void update(float deltaTime);
    void render();
    void destroy();
    
    bool importModel(const std::string& filename, Actor::Ptr targetActor = nullptr);
    
    // Serialization
    bool saveToFile(const std::string& filename);
    bool loadFromFile(const std::string& filename);
    
    // Root accessor (root actor)
    Actor::Ptr getRootNode() const { return rootNode; }
    
    bool updateSceneBuffers();
    bool hasBufferUpdateNeeded() const { return needsBufferUpdate; }
    
    const SceneDescriptor& getDescriptor() const { return descriptor; }
    void setDescriptor(const SceneDescriptor& desc) { descriptor = desc; }
    
    const std::string& getProjectPath() const { return projectPath; }
    void setProjectPath(const std::string& path) { projectPath = path; }
    
    static const std::string FILE_EXTENSION;
    
private:
    std::string name;
    SceneDescriptor descriptor;
    
    std::unordered_map<uint64_t, Actor::Ptr> actors;
    std::unordered_map<std::string, Actor::Ptr> actorsByName;
    
    std::vector<MeshComponent*> meshComponents;
    std::vector<PhysicsComponent*> physicsComponents;
    
    std::unique_ptr<physics::PhysicsWorld> physicsWorld;
    
    Actor::Ptr rootNode;
    
    std::string projectPath;
    
    bool needsBufferUpdate;
    
    void registerActor(Actor::Ptr actor);
    void unregisterActor(Actor::Ptr actor);
    void setupDefaultMaterial(class Material& material);
    
    void registerActorHierarchy(Actor::Ptr actor);
    void unregisterActorHierarchy(Actor::Ptr actor);
};

} // namespace ohao 