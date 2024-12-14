#include "selection_manager.hpp"
#include <algorithm>

namespace ohao {

void SelectionManager::setSelectedObject(SceneObject* object) {
    if (currentSelection != object) {
        currentSelection = object;

        // Clear multi-selection when single selecting
        multiSelection.clear();
        if (object) {
            multiSelection.push_back(object);
        }

        notifySelectionChanged();
    }
}

void SelectionManager::clearSelection() {
    currentSelection = nullptr;
    multiSelection.clear();
    notifySelectionChanged();
}

SceneObject* SelectionManager::getSelectedObject() const {
    return currentSelection;
}

void SelectionManager::addToSelection(SceneObject* object) {
    if (!object) return;

    if (std::find(multiSelection.begin(), multiSelection.end(), object) == multiSelection.end()) {
        multiSelection.push_back(object);
        currentSelection = object;  // Last selected becomes current
        notifySelectionChanged();
    }
}

void SelectionManager::removeFromSelection(SceneObject* object) {
    if (!object) return;

    auto it = std::find(multiSelection.begin(), multiSelection.end(), object);
    if (it != multiSelection.end()) {
        multiSelection.erase(it);

        // Update current selection
        if (currentSelection == object) {
            currentSelection = multiSelection.empty() ? nullptr : multiSelection.back();
        }

        notifySelectionChanged();
    }
}

bool SelectionManager::isSelected(SceneObject* object) const {
    if (!object) return false;
    return std::find(multiSelection.begin(), multiSelection.end(), object) != multiSelection.end();
}

const std::vector<SceneObject*>& SelectionManager::getSelection() const {
    return multiSelection;
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
