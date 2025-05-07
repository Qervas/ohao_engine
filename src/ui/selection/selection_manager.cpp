#include "selection_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include <algorithm>

namespace ohao {

void SelectionManager::setScene(Scene* newScene) {
    // If we're setting a new scene, clear the current selection
    clearSelection();
    
    // Set the new scene
    scene = newScene;
}

// Actor selection methods

void SelectionManager::setSelectedActor(Actor* actor) {
    // Clear current actor selection
    currentActor = nullptr;
    currentActorID = 0;
    selectedActors.clear();
    selectedIDs.clear();
    
    // Also clear the legacy selection
    currentSelection = nullptr;
    currentSelectionID = 0;
    multiSelection.clear();
    selectedObjects.clear();
    selectedObjectID = 0;
    isSelectionValid = false;
    
    // Then set the new selection
    if (actor != nullptr) {
        currentActor = actor;
        currentActorID = actor->getID();
        selectedActors.push_back(actor);
        selectedIDs.insert(actor->getID());
    }

    // Notify of the change
    notifySelectionChanged();
}

Actor* SelectionManager::getSelectedActor() const {
    return currentActor;
}

ObjectID SelectionManager::getSelectedID() const {
    return currentActorID;
}

void SelectionManager::addToSelection(Actor* actor) {
    if (!actor) return;

    ObjectID id = actor->getID();
    if (selectedIDs.find(id) == selectedIDs.end()) {
        selectedActors.push_back(actor);
                selectedIDs.insert(id);
        currentActor = actor;  // Last selected becomes current
        currentActorID = id;
        notifySelectionChanged();
            }
        }

void SelectionManager::removeFromSelection(Actor* actor) {
    if (!actor) return;
    
    ObjectID id = actor->getID();
    auto it = std::find(selectedActors.begin(), selectedActors.end(), actor);
    if (it != selectedActors.end()) {
        selectedActors.erase(it);
        selectedIDs.erase(id);

        // Update current selection
        if (currentActor == actor) {
            currentActor = selectedActors.empty() ? nullptr : selectedActors.back();
            currentActorID = selectedActors.empty() ? 0 : currentActor->getID();
    }
    
    notifySelectionChanged();
}
}

bool SelectionManager::isSelected(Actor* actor) const {
    if (!actor) return false;
    return selectedIDs.find(actor->getID()) != selectedIDs.end();
}

const std::vector<Actor*>& SelectionManager::getActorSelection() const {
    return selectedActors;
}

bool SelectionManager::isSelectedByID(ObjectID id) const {
    if (id == 0) return false;
    return selectedIDs.find(id) != selectedIDs.end();
}

const std::unordered_set<ObjectID>& SelectionManager::getSelectionIDs() const {
    return selectedIDs;
}

// SceneObject Backward Compatibility Methods

void SelectionManager::clearSelection() {
    // Clear both Actor and SceneObject selections
    currentActor = nullptr;
    currentActorID = 0;
    selectedActors.clear();
    selectedIDs.clear();
    
    currentSelection = nullptr;
    currentSelectionID = 0;
    multiSelection.clear();
    selectedObjects.clear();
    selectedObjectID = 0;
    isSelectionValid = false;
    
    notifySelectionChanged();
}

void SelectionManager::setSelectedObject(SceneObject* object) {
    // First clear all current selections
    clearSelection();
    
    // Then set the new selection for backward compatibility
    if (object != nullptr) {
        currentSelection = object;
        currentSelectionID = object->getID();
        multiSelection.push_back(object);
        selectedObjects.insert(object);
        
        // If this SceneObject is actually an Actor, also set the Actor selection
        if (auto actor = dynamic_cast<Actor*>(object)) {
            currentActor = actor;
            currentActorID = actor->getID();
            selectedActors.push_back(actor);
            selectedIDs.insert(actor->getID());
        }
    }

    // Notify of the change
    notifySelectionChanged();
}

SceneObject* SelectionManager::getSelectedObject() const {
    // Prefer the Actor system if available, otherwise fall back to legacy
    if (currentActor) {
        return currentActor;
    }
    return currentSelection;
}

ObjectID SelectionManager::getSelectedObjectID() const {
    // Prefer the Actor ID if available
    if (currentActorID != 0) {
        return currentActorID;
    }
    return currentSelectionID;
}

void SelectionManager::addToSelection(SceneObject* object) {
    if (!object) return;

    ObjectID id = object->getID();
    
    // Add to the legacy selection
    if (std::find(multiSelection.begin(), multiSelection.end(), object) == multiSelection.end()) {
        multiSelection.push_back(object);
        selectedObjects.insert(object);
        currentSelection = object;  // Last selected becomes current
        currentSelectionID = id;
    }
    
    // If this is an Actor, add to the Actor selection as well
    if (auto actor = dynamic_cast<Actor*>(object)) {
        if (std::find(selectedActors.begin(), selectedActors.end(), actor) == selectedActors.end()) {
            selectedActors.push_back(actor);
            selectedIDs.insert(id);
            currentActor = actor;
            currentActorID = id;
        }
    }
    
    notifySelectionChanged();
}

void SelectionManager::removeFromSelection(SceneObject* object) {
    if (!object) return;
    
    // Remove from legacy selection
    auto it = std::find(multiSelection.begin(), multiSelection.end(), object);
    if (it != multiSelection.end()) {
        multiSelection.erase(it);
        selectedObjects.erase(object);

        // Update current legacy selection
        if (currentSelection == object) {
            currentSelection = multiSelection.empty() ? nullptr : multiSelection.back();
            currentSelectionID = multiSelection.empty() ? 0 : currentSelection->getID();
        }
    }
    
    // If it's an Actor, also remove from Actor selection
    if (auto actor = dynamic_cast<Actor*>(object)) {
        auto actorIt = std::find(selectedActors.begin(), selectedActors.end(), actor);
        if (actorIt != selectedActors.end()) {
            selectedActors.erase(actorIt);
            selectedIDs.erase(actor->getID());
        
            // Update current Actor selection
            if (currentActor == actor) {
                currentActor = selectedActors.empty() ? nullptr : selectedActors.back();
                currentActorID = selectedActors.empty() ? 0 : currentActor->getID();
            }
        }
        }
        
        notifySelectionChanged();
}

bool SelectionManager::isSelected(SceneObject* object) const {
    if (!object) return false;
    
    // Check both systems - actor system first if it's an actor
    if (auto actor = dynamic_cast<Actor*>(object)) {
        return isSelected(actor);
}

    // Otherwise check the legacy system
    return selectedObjects.find(object) != selectedObjects.end();
}

const std::vector<SceneObject*>& SelectionManager::getObjectSelection() const {
    return multiSelection;
}

void SelectionManager::setSelectionChangedCallback(SelectionChangedCallback callback) {
    onSelectionChanged = callback;
}

void SelectionManager::notifySelectionChanged() {
    if (onSelectionChanged) {
        // Prefer Actor system if available
        if (currentActor) {
            onSelectionChanged(currentActor);
        } else {
        onSelectionChanged(currentSelection);
        }
    }
}

} // namespace ohao
