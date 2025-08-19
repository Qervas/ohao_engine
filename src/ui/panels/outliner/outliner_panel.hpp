#pragma once
#include "scene/scene_node.hpp"
#include "ui/common/panel_base.hpp"
#include "core/scene/scene.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/selection/selection_manager.hpp"
#include "core/actor/actor.hpp"
#include "core/component/mesh_component.hpp"
#include "core/component/physics_component.hpp"

namespace ohao {

class OutlinerPanel : public PanelBase {
public:
    enum class ViewMode {
        List,
        Graph
    };
    OutlinerPanel();
    void render() override;
    void setScene(Scene* scene) {
        currentScene = scene;
        OHAO_LOG_DEBUG("Outliner panel scene updated");
    }

private:
    Scene* currentScene{nullptr};
    SceneNode* selectedNode{nullptr};

    // Context menu state
    bool showContextMenu{false};
    SceneNode* contextMenuTarget{nullptr};

    ViewMode currentViewMode{ViewMode::List};
    bool showGraphView{false};
    float graphNodeSize{50.0f};
    float graphSpacingX{100.0f};
    float graphSpacingY{80.0f};
    ImVec2 graphOffset{0, 0};
    float graphZoom{1.0f};


    void renderSceneTree();
    void renderTreeNode(SceneNode* node);
    void handleDragAndDrop();
    void showObjectContextMenu(SceneNode* node);
    void handleObjectDeletion(SceneNode* node);
    void handleDelete();

    // Helper method to check if node is a SceneObject
    SceneObject* asSceneObject(SceneNode* node) const{
        return dynamic_cast<SceneObject*>(node);
    }

    void createPrimitiveObject(ohao::PrimitiveType type);
    std::shared_ptr<Model> generatePrimitiveMesh(ohao::PrimitiveType type);

    void renderGraphView();
    void renderGraphNode(SceneNode* node, ImVec2& pos, int depth);
    void drawNodeConnections(const ImVec2& start, const ImVec2& end);
    ImVec2 calculateNodePosition(int depth, int index);
    void handleGraphNavigation();
    void renderViewModeSelector();
    bool isRoot(SceneNode* node) const;

    bool isSceneObject(SceneNode* node) const;
    bool isSelected(SceneNode* node) const;


};

} // namespace ohao
