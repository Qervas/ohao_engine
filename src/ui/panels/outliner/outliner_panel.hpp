#pragma once
#include "scene/scene_node.hpp"
#include "ui/common/panel_base.hpp"
#include "core/scene/scene.hpp"
#include "ui/selection/selection_manager.hpp"

namespace ohao {

class OutlinerPanel : public PanelBase {
public:
    OutlinerPanel();
    void render() override;
    void setScene(Scene* scene) { currentScene = scene; }

private:
    void renderSceneTree();
    void renderTreeNode(SceneNode* node);
    void handleDragAndDrop();

    Scene* currentScene{nullptr};
    SceneNode* selectedNode{nullptr};

    // Context menu state
    bool showContextMenu{false};
    SceneNode* contextMenuTarget{nullptr};

    void showObjectContextMenu(SceneNode* node);
    void handleObjectDeletion(SceneNode* node);

    // Helper method to check if node is a SceneObject
    SceneObject* asSceneObject(SceneNode* node) {
        return dynamic_cast<SceneObject*>(node);
    }
};

} // namespace ohao
