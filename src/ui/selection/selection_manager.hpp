#pragma once
#include <vector>
#include <functional>
#include <unordered_set>
#include "core/scene/scene.hpp"

namespace ohao {

class SelectionManager {
public:
    static SelectionManager& get() {
        static SelectionManager instance;
        return instance;
    }

    void setSelectedObject(SceneObject* object);
    void clearSelection();
    SceneObject* getSelectedObject() const;

    // ID-based selection methods
    void selectByID(ObjectID id);
    ObjectID getSelectedObjectID() const;

    // Multi-selection support
    void addToSelection(SceneObject* object);
    void addToSelectionByID(ObjectID id);
    void removeFromSelection(SceneObject* object);
    void removeFromSelectionByID(ObjectID id);
    bool isSelected(SceneObject* object) const;
    bool isSelectedByID(ObjectID id) const;
    const std::vector<SceneObject*>& getSelection() const;
    const std::unordered_set<ObjectID>& getSelectionIDs() const;

    // Selection changed callback
    using SelectionChangedCallback = std::function<void(SceneObject*)>;
    void setSelectionChangedCallback(SelectionChangedCallback callback);

private:
    SelectionManager() = default;
    ~SelectionManager() = default;

    void notifySelectionChanged();

    SceneObject* currentSelection{nullptr};
    ObjectID currentSelectionID{0};
    std::vector<SceneObject*> multiSelection;
    std::unordered_set<ObjectID> selectedIDs;
    SelectionChangedCallback onSelectionChanged;

    // Prevent copying
    SelectionManager(const SelectionManager&) = delete;
    SelectionManager& operator=(const SelectionManager&) = delete;
};

} // namespace ohao
