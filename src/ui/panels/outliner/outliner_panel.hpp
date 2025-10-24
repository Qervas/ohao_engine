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
    SceneObject* selectedObject{nullptr};

    // Context menu state
    bool showContextMenu{false};
    SceneObject* contextMenuTargetObject{nullptr};
    Actor* pendingDeleteActor{nullptr};

    // Removed list/graph view modes; outliner shows actor hierarchy only


    void renderActorList();
    void handleDragAndDrop();
    void showObjectContextMenu(SceneObject* object);
    void handleObjectDeletion(SceneObject* object);
    void handleDelete();

    // Helper method to check if object is a SceneObject
    SceneObject* asSceneObject(SceneObject* object) const{
        return dynamic_cast<SceneObject*>(object);
    }

    void createPrimitiveObject(ohao::PrimitiveType type);
    std::shared_ptr<Model> generatePrimitiveMesh(ohao::PrimitiveType type);

    // Graph/list view code removed
    bool isRoot(SceneObject* object) const;

    bool isSceneObject(SceneObject* object) const;
    bool isSelected(SceneObject* object) const;


};

} // namespace ohao
