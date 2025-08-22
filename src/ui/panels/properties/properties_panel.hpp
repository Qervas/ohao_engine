#pragma once
#include "ui/common/panel_base.hpp"
#include "core/scene/scene.hpp"
#include "ui/selection/selection_manager.hpp"
#include "core/actor/actor.hpp"
#include "core/component/transform_component.hpp"
#include "core/component/mesh_component.hpp"
#include "core/component/material_component.hpp"
#include "core/component/physics_component.hpp"
#include "core/component/light_component.hpp"
#include "core/material/material.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ohao {

class PropertiesPanel : public PanelBase {
public:
    PropertiesPanel();
    void render() override;
    
    void setScene(Scene* scene) { 
        currentScene = scene; 
        // Update selection manager if needed
        if (SelectionManager::get().getSelectedObject() == nullptr && scene) {
            SelectionManager::get().setScene(scene);
        }
    }
    
private:
    // Core rendering methods
    void renderNodeProperties(SceneNode* node);
    void renderTransformProperties(SceneNode* node);
    
    // Actor-Component system methods
    void renderActorProperties(Actor* actor);
    void renderTransformComponentProperties(TransformComponent* transform);
    void renderComponentProperties(Actor* actor);
    
    // Component-specific property editors
    void renderMeshComponentProperties(MeshComponent* component);
    void renderMaterialComponentProperties(MaterialComponent* component);
    void renderPhysicsComponentProperties(PhysicsComponent* component);
    void renderLightComponentProperties(LightComponent* component);
    void renderPBRMaterialProperties(Material& material);

    // UI Helper methods
    bool renderVec3Control(const std::string& label, glm::vec3& values,
                          float resetValue = 0.0f);

    // Helper methods for checking node types
    SceneObject* asSceneObject(SceneNode* node) {
        return dynamic_cast<SceneObject*>(node);
    }
    
    Actor* asActor(SceneNode* node) {
        return dynamic_cast<Actor*>(node);
    }

    // Cached values for undo/redo
    glm::vec3 lastPosition{0.0f};
    glm::vec3 lastRotation{0.0f};
    glm::vec3 lastScale{1.0f};
    
    // Scene reference
    Scene* currentScene{nullptr};
    
    // UI state
    bool showErrorPopup{false};
    std::string errorMessage;

    // Primitive types for mesh generation
    enum class PrimitiveType {
        Empty,
        Cube,
        Sphere,
        Platform,
        Cylinder,
        Cone
    };

    // Generate primitive meshes for MeshComponent
    std::shared_ptr<Model> generatePrimitiveMesh(PrimitiveType type);
};

} // namespace ohao
