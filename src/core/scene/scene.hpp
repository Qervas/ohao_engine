#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <iostream>
#include "../actor/actor.hpp"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "scene_change_tracker.hpp"

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
    
    // Private creation helper to work around protected constructor
    class SceneFactory {
    private:
        friend class Scene;
        
    public:
        // Factory method for creating scenes
        static Scene::Ptr create(const std::string& name = "New Scene") {
            auto scene = std::shared_ptr<Scene>(new Scene(name));
            if (!scene->setupRootNode()) {
                return nullptr;
            }
            return scene;
        }
    };
    
    // Static factory method for proper construction
    static Scene::Ptr create(const std::string& name = "New Scene") {
        return SceneFactory::create(name);
    }
    
    // Disable copying
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    
    // Constructor is now protected - use create() factory method
protected:
    Scene(const std::string& name = "New Scene");
public:
    ~Scene();
    
    // Explicit initialization to be called after construction 
    // This ensures proper setup of the scene
    bool setupRootNode();
    
    // Scene management
    void setName(const std::string& name);
    const std::string& getName() const;
    void setDirty(bool dirty = true);
    bool isDirty() const;
    
    // Actor management
    void addActor(std::shared_ptr<Actor> actor);
    void removeActor(std::shared_ptr<Actor> actor);
    void removeActor(const std::string& name);
    void removeActor(uint64_t id);
    void removeAllActors();
    std::shared_ptr<Actor> getActor(const std::string& name);
    const std::vector<std::shared_ptr<Actor>>& getActors() const;
    Actor::Ptr createActor(const std::string& name);
    
    // Root node access - now has validation and safe handling
    std::shared_ptr<Actor> getRootNode() const;
    bool hasValidRoot() const;
    
    // Scene reset/cleanup - clears everything but maintains a valid root
    void reset();
    
    // Scene lifecycle methods
    void initialize(); // Initialize all actors 
    void reinitialize(bool forceNewRoot = false); // Recreate root node if needed
    void update(float deltaTime);
    void render();
    void destroy();
    
    // Project path management
    void setProjectPath(const std::string& path) { projectPath = path; }
    const std::string& getProjectPath() const { return projectPath; }
    std::string getProjectDirPath() const;
    
    // Import methods
    bool importModel(const std::string& filename, Actor::Ptr targetActor = nullptr);
    
    // File operations
    bool saveToFile(const std::string& filePath);
    static bool loadFromFile(const std::string& filePath, Scene::Ptr& outScene);
    
    // Actor lookup
    Actor::Ptr findActor(const std::string& name) const;
    Actor::Ptr findActor(uint64_t id) const;
    std::vector<Actor::Ptr> findActorsByName(const std::string& partialName) const;
    std::vector<Actor::Ptr> findActorsByTag(const std::string& tag) const;
    const std::unordered_map<uint64_t, Actor::Ptr>& getAllActors() const;
    
    // Change tracking
    SceneChangeTracker* getChangeTracker() const { return changeTracker.get(); }
    void beginModification();
    void endModification();
    void trackActorAdded(std::shared_ptr<Actor> actor);
    void trackActorRemoved(std::shared_ptr<Actor> actor);
    void trackActorModified(Actor* actor, const nlohmann::json& oldState, const nlohmann::json& newState);
    void trackComponentModified(Component* component, const nlohmann::json& oldState, const nlohmann::json& newState);
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void clearHistory();
    void saveHistory(const std::string& filename) const;
    void loadHistory(const std::string& filename);
    std::string getLastChangeDescription() const;
    std::vector<std::string> getChangeHistory() const;
    
    // Serialization
    nlohmann::json serialize() const;
    void deserialize(const nlohmann::json& data);
    
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
    const SceneDescriptor& getDescriptor() const { return descriptor; }
    void setDescriptor(const SceneDescriptor& desc) { descriptor = desc; }
    
    // Scene file extension
    static const std::string FILE_EXTENSION;
    
    // Light management
    void addLight(const std::string& name, const Light& light);
    void removeLight(const std::string& name);
    void updateLight(const std::string& name, const Light& light);
    Light* getLight(const std::string& name);
    const std::unordered_map<std::string, Light>& getAllLights() const;
    
    // Buffer update
    bool updateSceneBuffers();
    bool hasBufferUpdateNeeded() const { return needsBufferUpdate; }
    
    // Scene state tracking
    void clearDirty();
    
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
    bool dirty{false}; // Track if scene has unsaved changes
    
    // Helper methods
    void registerActor(Actor::Ptr actor);
    void unregisterActor(Actor::Ptr actor);
    void setupDefaultMaterial(class Material& material);
    
    // Hierarchy helpers
    void registerActorHierarchy(Actor::Ptr actor);
    void unregisterActorHierarchy(Actor::Ptr actor);
    
    std::unique_ptr<SceneChangeTracker> changeTracker;
};

} // namespace ohao 