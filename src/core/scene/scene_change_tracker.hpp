#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace ohao {

// Forward declarations
class Scene;
class Actor;
class Component;

// Base class for all scene changes
class SceneChange {
public:
    virtual ~SceneChange() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual std::string getDescription() const = 0;
    virtual nlohmann::json serialize() const = 0;
    virtual void deserialize(const nlohmann::json& data) = 0;
    
    // Timestamp for when the change was made
    std::chrono::system_clock::time_point timestamp;
};

// Forward declarations of change types
class ActorAddedChange;
class ActorRemovedChange;
class ComponentModifiedChange;
class ActorModifiedChange;

// Specific change types
class ActorAddedChange : public SceneChange {
public:
    ActorAddedChange(Scene* scene, std::shared_ptr<Actor> actor);
    void execute() override;
    void undo() override;
    void redo() override;
    std::string getDescription() const override;
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    Scene* scene;
    std::shared_ptr<Actor> actor;
};

class ActorRemovedChange : public SceneChange {
public:
    ActorRemovedChange(Scene* scene, std::shared_ptr<Actor> actor);
    void execute() override;
    void undo() override;
    void redo() override;
    std::string getDescription() const override;
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    Scene* scene;
    std::shared_ptr<Actor> actor;
};

class ComponentModifiedChange : public SceneChange {
public:
    ComponentModifiedChange(Scene* scene, Component* component, const nlohmann::json& oldState, const nlohmann::json& newState);
    void execute() override;
    void undo() override;
    void redo() override;
    std::string getDescription() const override;
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    Scene* scene;
    Component* component;
    nlohmann::json oldState;
    nlohmann::json newState;
};

class ActorModifiedChange : public SceneChange {
public:
    ActorModifiedChange(Scene* scene, Actor* actor, const nlohmann::json& oldState, const nlohmann::json& newState);
    void execute() override;
    void undo() override;
    void redo() override;
    std::string getDescription() const override;
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    Scene* scene;
    Actor* actor;
    nlohmann::json oldState;
    nlohmann::json newState;
};

class SceneChangeTracker {
public:
    SceneChangeTracker(Scene* scene);
    
    // Change management
    void addChange(std::unique_ptr<SceneChange> change);
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    
    // History management
    void clearHistory();
    void saveHistory(const std::string& filename) const;
    void loadHistory(const std::string& filename);
    
    // State tracking
    bool isDirty() const;
    void clearDirty();
    size_t getChangeCount() const;
    
    // Change description
    std::string getLastChangeDescription() const;
    std::vector<std::string> getChangeHistory() const;
    
private:
    Scene* scene;
    std::vector<std::unique_ptr<SceneChange>> changes;
    size_t currentChangeIndex;
    bool dirty;
    
    // Helper methods
    void trimRedoStack();
    void markDirty();
};

} // namespace ohao 