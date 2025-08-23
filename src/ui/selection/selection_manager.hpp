#pragma once
#include <vector>
#include <functional>
#include <unordered_set>
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"

namespace ohao {

class SceneObject;
class Scene;
class Actor;

using ObjectID = uint64_t;
using SelectionChangedCallback = std::function<void(void*)>;

class SelectionManager {
public:
    static SelectionManager& get() {
        static SelectionManager instance;
        return instance;
    }

    // Primary selection methods for Actors
    void setSelectedActor(Actor* actor);
    void clearSelection();
    Actor* getSelectedActor() const;
    ObjectID getSelectedID() const;

    // Multi-selection for Actors
    void addToSelection(Actor* actor);
    void removeFromSelection(Actor* actor);
    bool isSelected(Actor* actor) const;
    bool isSelectedByID(ObjectID id) const;
    const std::vector<Actor*>& getActorSelection() const;
    const std::unordered_set<ObjectID>& getSelectionIDs() const;

    // Backward compatibility for SceneObjects
    void setSelectedObject(SceneObject* object);
    SceneObject* getSelectedObject() const;
    ObjectID getSelectedObjectID() const;
    void addToSelection(SceneObject* object);
    void removeFromSelection(SceneObject* object);
    bool isSelected(SceneObject* object) const;
    const std::vector<SceneObject*>& getObjectSelection() const;

    // Scene management
    void setScene(Scene* scene);

    // Event notification
    void setSelectionChangedCallback(SelectionChangedCallback callback);

private:
    SelectionManager() = default;
    ~SelectionManager() = default;
    SelectionManager(const SelectionManager&) = delete;
    SelectionManager& operator=(const SelectionManager&) = delete;

    void notifySelectionChanged();

    // Current scene
    Scene* scene = nullptr;

    // Main Actor selection
    Actor* currentActor = nullptr;
    ObjectID currentActorID = 0;

    // Multi-selection support for Actors
    std::vector<Actor*> selectedActors;
    std::unordered_set<ObjectID> selectedIDs;

    // Legacy support for SceneObject
    SceneObject* currentSelection = nullptr;
    ObjectID currentSelectionID = 0;
    std::vector<SceneObject*> multiSelection;
    std::unordered_set<SceneObject*> selectedObjects;
    ObjectID selectedObjectID = 0;
    bool isSelectionValid = false;

    // Callback
    SelectionChangedCallback onSelectionChanged;
};

} // namespace ohao
