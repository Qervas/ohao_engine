#pragma once

#include "ui/common/panel_base.hpp"
#include "renderer/components/light_component.hpp"
#include "engine/actor/actor.hpp"
#include <glm/glm.hpp>

namespace ohao {

/**
 * Dedicated panel for editing LightComponent properties
 * Shows light type, color, intensity, range, direction, and cone angles
 */
class LightComponentPanel : public PanelBase {
public:
    LightComponentPanel();
    void render() override;

    void setSelectedActor(Actor* actor) { m_selectedActor = actor; }
    Actor* getSelectedActor() const { return m_selectedActor; }

private:
    void renderLightProperties(LightComponent* component);
    bool renderVec3Control(const std::string& label, glm::vec3& values, float resetValue = 0.0f);

    Actor* m_selectedActor{nullptr};
};

} // namespace ohao
