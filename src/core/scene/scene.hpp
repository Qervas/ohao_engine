#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include "../actor/actor.hpp"
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

struct Light {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    bool enabled{true};
};

class Scene {
public:
    using Ptr = std::shared_ptr<Scene>;
    
    Scene(const std::string& name = "New Scene");
    ~Scene();
    
    // Actor management
    Actor::Ptr createActor(const std::string& name = "Actor");
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
    
    // Scene properties
    const std::string& getName() const;
    void setName(const std::string& name);
    
    // Light management
    void addLight(const std::string& name, const Light& light);
    void removeLight(const std::string& name);
    void updateLight(const std::string& name, const Light& light);
    Light* getLight(const std::string& name);
    const std::unordered_map<std::string, Light>& getAllLights() const;
    
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
    
    // Lighting
    std::unordered_map<std::string, Light> lights;
    
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