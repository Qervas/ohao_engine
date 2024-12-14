#pragma once
#include <vector>
#include <functional>
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

    // Multi-selection support
    void addToSelection(SceneObject* object);
    void removeFromSelection(SceneObject* object);
    bool isSelected(SceneObject* object) const;
    const std::vector<SceneObject*>& getSelection() const;

    // Selection changed callback
    using SelectionChangedCallback = std::function<void(SceneObject*)>;
    void setSelectionChangedCallback(SelectionChangedCallback callback);

private:
    SelectionManager() = default;
    ~SelectionManager() = default;

    void notifySelectionChanged();

    SceneObject* currentSelection{nullptr};
    std::vector<SceneObject*> multiSelection;
    SelectionChangedCallback onSelectionChanged;

    // Prevent copying
    SelectionManager(const SelectionManager&) = delete;
    SelectionManager& operator=(const SelectionManager&) = delete;
};

} // namespace ohao
