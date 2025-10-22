#pragma once
#include "engine/scene/scene_object.hpp"
#include "ui/common/panel_base.hpp"
#include "engine/scene/scene.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/selection/selection_manager.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"

namespace ohao {

class OutlinerPanel : public PanelBase {
public:
    OutlinerPanel();
    void render() override;
    void setScene(Scene* scene) {
        currentScene = scene;
        OHAO_LOG_DEBUG("Outliner panel scene updated");
    }

private:
    Scene* currentScene{nullptr};
    SceneObject* selectedNode{nullptr};

    // Context menu state
    bool showContextMenu{false};
    SceneObject* contextMenuTarget{nullptr};

    // Removed list/graph view modes; outliner shows actor hierarchy only


    void renderActorList();
    void handleDragAndDrop();
    void showObjectContextMenu(SceneObject* node);
    void handleObjectDeletion(SceneObject* node);
    void handleDelete();

    // Helper method to check if node is a SceneObject
    SceneObject* asSceneObject(SceneObject* node) const{
        return dynamic_cast<SceneObject*>(node);
    }

    void createPrimitiveObject(ohao::PrimitiveType type);
    std::shared_ptr<Model> generatePrimitiveMesh(ohao::PrimitiveType type);

    // Graph/list view code removed
    bool isRoot(SceneObject* node) const;

    bool isSceneObject(SceneObject* node) const;
    bool isSelected(SceneObject* node) const;


};

} // namespace ohao
