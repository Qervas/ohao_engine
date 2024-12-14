#pragma once
#include "ui/common/panel_base.hpp"
#include "core/scene/scene.hpp"
#include "ui/selection/selection_manager.hpp"

namespace ohao {

class PropertiesPanel : public PanelBase {
public:
    PropertiesPanel();
    void render() override;

private:
    void renderNodeProperties(SceneNode* node);
    void renderTransformProperties(SceneNode* node);
    void renderMaterialProperties(SceneObject* object);
    void renderComponentProperties(SceneObject* object);


    void renderVec3Control(const std::string& label, glm::vec3& values,
                          float resetValue = 0.0f);

    SceneObject* asSceneObject(SceneNode* node) {
        return dynamic_cast<SceneObject*>(node);
    }

    // Cached values for undo/redo
    glm::vec3 lastPosition{0.0f};
    glm::vec3 lastRotation{0.0f};
    glm::vec3 lastScale{1.0f};
};

} // namespace ohao
