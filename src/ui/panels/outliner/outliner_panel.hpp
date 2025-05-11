#pragma once
#include "scene/scene_node.hpp"
#include "ui/common/panel_base.hpp"
#include "core/scene/scene.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/selection/selection_manager.hpp"
#include "core/actor/actor.hpp"
#include "core/actor/light_actor.hpp"
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
    enum class PrimitiveType {
        Empty,
        Cube,
        Sphere,
        Plane,
        Cylinder,
        Cone
    };

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

    // Core rendering methods
    void renderSceneTree();
    void renderTreeNode(SceneNode* node);
    void renderOrphanedActors();
    void handleDragAndDrop(SceneNode* targetNode, const ImGuiPayload* payload);
    void handleGlobalDragAndDrop(); // Legacy method for backward compatibility
    void showObjectContextMenu(SceneNode* node);
    
    // Object management methods
    void handleObjectDeletion(SceneNode* node);
    void handleDelete();
    void createPrimitiveObject(PrimitiveType type);
    void createLightActor(LightActor::LightActorType type);
    std::shared_ptr<Model> generatePrimitiveMesh(PrimitiveType type);
    std::string generateUniqueName(const std::string& baseName);

    // Graph view methods
    void renderGraphView();
    void renderActorNode(Actor* actor, ImVec2& pos);
    void drawNodeConnections(const ImVec2& start, const ImVec2& end);
    ImVec2 calculateNodePosition(int depth, int index);
    void handleGraphNavigation();
    void renderViewModeSelector();
    
    // Type safety helpers
    bool isRoot(SceneNode* node) const;
    bool isSceneObject(SceneNode* node) const;
    SceneObject* asSceneObject(SceneNode* node) const;
    bool isSelected(SceneNode* node) const;
};

} // namespace ohao
