#pragma once

#include "ui/common/panel_base.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/actor/actor.hpp"
#include "engine/scene/scene.hpp"
#include <glm/glm.hpp>

namespace ohao {

/**
 * Dedicated panel for editing PhysicsComponent properties
 * Shows rigid body settings, collision shapes, forces, and material properties
 */
class PhysicsComponentPanel : public PanelBase {
public:
    PhysicsComponentPanel();
    void render() override;

    void setSelectedActor(Actor* actor) { m_selectedActor = actor; }
    Actor* getSelectedActor() const { return m_selectedActor; }

    void setScene(Scene* scene) { m_currentScene = scene; }
    Scene* getScene() const { return m_currentScene; }

private:
    void renderPhysicsProperties(PhysicsComponent* component);
    bool renderVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f);

    Actor* m_selectedActor{nullptr};
    Scene* m_currentScene{nullptr};
};

} // namespace ohao
