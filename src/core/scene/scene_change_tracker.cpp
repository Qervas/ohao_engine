#include "scene_change_tracker.hpp"
#include "scene.hpp"
#include "../actor/actor.hpp"
#include "../component/component.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace ohao {

// SceneChangeTracker implementation
SceneChangeTracker::SceneChangeTracker(Scene* scene)
    : scene(scene)
    , currentChangeIndex(0)
    , dirty(false)
{
}

void SceneChangeTracker::addChange(std::unique_ptr<SceneChange> change) {
    // Trim any redo stack
    trimRedoStack();
    
    // Add the new change
    change->timestamp = std::chrono::system_clock::now();
    changes.push_back(std::move(change));
    currentChangeIndex = changes.size();
    
    // Mark as dirty
    markDirty();
}

void SceneChangeTracker::undo() {
    if (!canUndo()) return;
    
    currentChangeIndex--;
    changes[currentChangeIndex]->undo();
    markDirty();
}

void SceneChangeTracker::redo() {
    if (!canRedo()) return;
    
    changes[currentChangeIndex]->redo();
    currentChangeIndex++;
    markDirty();
}

bool SceneChangeTracker::canUndo() const {
    return currentChangeIndex > 0;
}

bool SceneChangeTracker::canRedo() const {
    return currentChangeIndex < changes.size();
}

void SceneChangeTracker::clearHistory() {
    changes.clear();
    currentChangeIndex = 0;
    dirty = false;
}

void SceneChangeTracker::saveHistory(const std::string& filename) const {
    nlohmann::json history;
    history["changes"] = nlohmann::json::array();
    
    for (const auto& change : changes) {
        nlohmann::json changeData;
        changeData["type"] = typeid(*change).name();
        changeData["data"] = change->serialize();
        changeData["timestamp"] = std::chrono::system_clock::to_time_t(change->timestamp);
        history["changes"].push_back(changeData);
    }
    
    std::ofstream file(filename);
    file << history.dump(4);
}

void SceneChangeTracker::loadHistory(const std::string& filename) {
    // Clear existing history
    changes.clear();
    currentChangeIndex = 0;
    
    // Create a basic change from default arguments
    // Instead of std::make_unique<SceneChange>(), use a concrete class like ActorAddedChange
    auto defaultScene = scene ? scene : nullptr;
    auto defaultActor = std::make_shared<Actor>("Default");
    auto change = std::make_unique<ActorAddedChange>(defaultScene, defaultActor);
    
    try {
        // Load from file
        nlohmann::json data;
        std::ifstream file(filename);
        if (file.is_open()) {
            file >> data;
            file.close();
            
            if (data.contains("changes") && data["changes"].is_array()) {
                for (const auto& changeData : data["changes"]) {
                    // Instead of creating an abstract base class, create a concrete derived class
                    // based on the change type stored in the JSON
                    if (changeData.contains("type")) {
                        std::string type = changeData["type"];
                        std::unique_ptr<SceneChange> loadedChange;
                        
                        if (type == "ActorAdded") {
                            loadedChange = std::make_unique<ActorAddedChange>(scene, nullptr);
                        } else if (type == "ActorRemoved") {
                            loadedChange = std::make_unique<ActorRemovedChange>(scene, nullptr);
                        } else if (type == "ComponentModified") {
                            loadedChange = std::make_unique<ComponentModifiedChange>(scene, nullptr, nlohmann::json(), nlohmann::json());
                        } else if (type == "ActorModified") {
                            loadedChange = std::make_unique<ActorModifiedChange>(scene, nullptr, nlohmann::json(), nlohmann::json());
                        } else {
                            // Skip unknown change types
                            continue;
                        }
                        
                        loadedChange->deserialize(changeData);
                        changes.push_back(std::move(loadedChange));
                    }
                }
            }
            
            // Set current index to the end of the history
            currentChangeIndex = changes.size();
        }
    } catch (const std::exception& e) {
        // Failed to load history
        // Just continue with empty history
    }
}

bool SceneChangeTracker::isDirty() const {
    return dirty;
}

void SceneChangeTracker::clearDirty() {
    dirty = false;
}

size_t SceneChangeTracker::getChangeCount() const {
    return changes.size();
}

std::string SceneChangeTracker::getLastChangeDescription() const {
    if (changes.empty()) return "No changes";
    return changes.back()->getDescription();
}

std::vector<std::string> SceneChangeTracker::getChangeHistory() const {
    std::vector<std::string> history;
    history.reserve(changes.size());
    
    for (const auto& change : changes) {
        history.push_back(change->getDescription());
    }
    
    return history;
}

void SceneChangeTracker::trimRedoStack() {
    if (currentChangeIndex < changes.size()) {
        changes.erase(changes.begin() + currentChangeIndex, changes.end());
    }
}

void SceneChangeTracker::markDirty() {
    dirty = true;
    if (scene) {
        scene->setDirty();
    }
}

// ActorAddedChange implementation
ActorAddedChange::ActorAddedChange(Scene* scene, std::shared_ptr<Actor> actor)
    : scene(scene)
    , actor(actor)
{
}

void ActorAddedChange::execute() {
    if (scene && actor) {
        scene->addActor(actor);
    }
}

void ActorAddedChange::undo() {
    if (scene && actor) {
        scene->removeActor(actor);
    }
}

void ActorAddedChange::redo() {
    execute();
}

std::string ActorAddedChange::getDescription() const {
    return "Added actor: " + (actor ? actor->getName() : "Unknown");
}

nlohmann::json ActorAddedChange::serialize() const {
    nlohmann::json data;
    if (actor) {
        data["actor_id"] = actor->getID();
        data["actor_name"] = actor->getName();
    }
    return data;
}

void ActorAddedChange::deserialize(const nlohmann::json& data) {
    // TODO: Implement proper actor deserialization
}

// ActorRemovedChange implementation
ActorRemovedChange::ActorRemovedChange(Scene* scene, std::shared_ptr<Actor> actor)
    : scene(scene)
    , actor(actor)
{
}

void ActorRemovedChange::execute() {
    if (scene && actor) {
        scene->removeActor(actor);
    }
}

void ActorRemovedChange::undo() {
    if (scene && actor) {
        scene->addActor(actor);
    }
}

void ActorRemovedChange::redo() {
    execute();
}

std::string ActorRemovedChange::getDescription() const {
    return "Removed actor: " + (actor ? actor->getName() : "Unknown");
}

nlohmann::json ActorRemovedChange::serialize() const {
    nlohmann::json data;
    if (actor) {
        data["actor_id"] = actor->getID();
        data["actor_name"] = actor->getName();
    }
    return data;
}

void ActorRemovedChange::deserialize(const nlohmann::json& data) {
    // TODO: Implement proper actor deserialization
}

// ComponentModifiedChange implementation
ComponentModifiedChange::ComponentModifiedChange(Scene* scene, Component* component, const nlohmann::json& oldState, const nlohmann::json& newState)
    : scene(scene)
    , component(component)
    , oldState(oldState)
    , newState(newState)
{
}

void ComponentModifiedChange::execute() {
    if (component) {
        component->deserialize(newState);
    }
}

void ComponentModifiedChange::undo() {
    if (component) {
        component->deserialize(oldState);
    }
}

void ComponentModifiedChange::redo() {
    execute();
}

std::string ComponentModifiedChange::getDescription() const {
    std::string desc = "Modified component: ";
    desc += (component ? component->getTypeName() : "Unknown");
    return desc;
}

nlohmann::json ComponentModifiedChange::serialize() const {
    nlohmann::json data;
    if (component) {
        data["component_type"] = component->getTypeName();
        data["old_state"] = oldState;
        data["new_state"] = newState;
    }
    return data;
}

void ComponentModifiedChange::deserialize(const nlohmann::json& data) {
    // TODO: Implement proper component deserialization
}

// ActorModifiedChange implementation
ActorModifiedChange::ActorModifiedChange(Scene* scene, Actor* actor, const nlohmann::json& oldState, const nlohmann::json& newState)
    : scene(scene)
    , actor(actor)
    , oldState(oldState)
    , newState(newState)
{
}

void ActorModifiedChange::execute() {
    if (actor) {
        actor->deserialize(newState);
    }
}

void ActorModifiedChange::undo() {
    if (actor) {
        actor->deserialize(oldState);
    }
}

void ActorModifiedChange::redo() {
    execute();
}

std::string ActorModifiedChange::getDescription() const {
    std::string desc = "Modified actor: ";
    desc += (actor ? actor->getName() : "Unknown");
    return desc;
}

nlohmann::json ActorModifiedChange::serialize() const {
    nlohmann::json data;
    if (actor) {
        data["actor_id"] = actor->getID();
        data["actor_name"] = actor->getName();
        data["old_state"] = oldState;
        data["new_state"] = newState;
    }
    return data;
}

void ActorModifiedChange::deserialize(const nlohmann::json& data) {
    // TODO: Implement proper actor deserialization
}

} // namespace ohao 