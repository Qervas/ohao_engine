#include "selection_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include <algorithm>

namespace ohao {

void SelectionManager::setSelectedObject(SceneObject* object) {
    // First clear the current selection to avoid any cross-reference issues
    currentSelection = nullptr;
    currentSelectionID = 0;
    multiSelection.clear();
    selectedIDs.clear();
    
    // Then set the new selection
    if (object != nullptr) {
        currentSelection = object;
        currentSelectionID = object->getID();
        multiSelection.push_back(object);
        selectedIDs.insert(object->getID());
    }

    // Notify of the change
    notifySelectionChanged();
}

void SelectionManager::selectByID(ObjectID id) {
    // First clear the current selection to avoid any cross-reference issues
    currentSelection = nullptr;
    currentSelectionID = 0;
    multiSelection.clear();
    selectedIDs.clear();
    
    if (id != 0) {
        // Look up the object by ID in active scene
        auto* vulkanContext = VulkanContext::getContextInstance();
        if (vulkanContext && vulkanContext->getScene()) {
            auto scene = vulkanContext->getScene();
            auto obj = scene->getObjectByID(id);
            if (obj) {
                currentSelection = obj.get();
                currentSelectionID = id;
                multiSelection.push_back(obj.get());
                selectedIDs.insert(id);
            }
        }
    }
    
    notifySelectionChanged();
}

void SelectionManager::clearSelection() {
    currentSelection = nullptr;
    currentSelectionID = 0;
    multiSelection.clear();
    selectedIDs.clear();
    notifySelectionChanged();
}

SceneObject* SelectionManager::getSelectedObject() const {
    return currentSelection;
}

ObjectID SelectionManager::getSelectedObjectID() const {
    return currentSelectionID;
}

void SelectionManager::addToSelection(SceneObject* object) {
    if (!object) return;

    ObjectID id = object->getID();
    if (selectedIDs.find(id) == selectedIDs.end()) {
        multiSelection.push_back(object);
        selectedIDs.insert(id);
        currentSelection = object;  // Last selected becomes current
        currentSelectionID = id;
        notifySelectionChanged();
    }
}

void SelectionManager::addToSelectionByID(ObjectID id) {
    if (id == 0 || selectedIDs.find(id) != selectedIDs.end()) return;
    
    // Look up the object by ID in active scene
    auto* vulkanContext = VulkanContext::getContextInstance();
    if (vulkanContext && vulkanContext->getScene()) {
        auto scene = vulkanContext->getScene();
        auto obj = scene->getObjectByID(id);
        if (obj) {
            multiSelection.push_back(obj.get());
            selectedIDs.insert(id);
            currentSelection = obj.get();
            currentSelectionID = id;
            notifySelectionChanged();
        }
    }
}

void SelectionManager::removeFromSelection(SceneObject* object) {
    if (!object) return;
    
    ObjectID id = object->getID();
    auto it = std::find(multiSelection.begin(), multiSelection.end(), object);
    if (it != multiSelection.end()) {
        multiSelection.erase(it);
        selectedIDs.erase(id);

        // Update current selection
        if (currentSelection == object) {
            currentSelection = multiSelection.empty() ? nullptr : multiSelection.back();
            currentSelectionID = multiSelection.empty() ? 0 : currentSelection->getID();
        }

        notifySelectionChanged();
    }
}

void SelectionManager::removeFromSelectionByID(ObjectID id) {
    if (id == 0) return;
    
    if (selectedIDs.find(id) != selectedIDs.end()) {
        selectedIDs.erase(id);
        
        // Remove from multiSelection vector
        auto it = std::find_if(multiSelection.begin(), multiSelection.end(), 
                              [id](SceneObject* obj) { return obj->getID() == id; });
        if (it != multiSelection.end()) {
            multiSelection.erase(it);
        }
        
        // Update current selection
        if (currentSelectionID == id) {
            currentSelection = multiSelection.empty() ? nullptr : multiSelection.back();
            currentSelectionID = multiSelection.empty() ? 0 : currentSelection->getID();
        }
        
        notifySelectionChanged();
    }
}

bool SelectionManager::isSelected(SceneObject* object) const {
    if (!object) return false;
    return selectedIDs.find(object->getID()) != selectedIDs.end();
}

bool SelectionManager::isSelectedByID(ObjectID id) const {
    if (id == 0) return false;
    return selectedIDs.find(id) != selectedIDs.end();
}

const std::vector<SceneObject*>& SelectionManager::getSelection() const {
    return multiSelection;
}

const std::unordered_set<ObjectID>& SelectionManager::getSelectionIDs() const {
    return selectedIDs;
}

void SelectionManager::setSelectionChangedCallback(SelectionChangedCallback callback) {
    onSelectionChanged = callback;
}

void SelectionManager::notifySelectionChanged() {
    if (onSelectionChanged) {
        onSelectionChanged(currentSelection);
    }
}

} // namespace ohao
