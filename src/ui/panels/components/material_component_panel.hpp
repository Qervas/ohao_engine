#pragma once

#include "ui/common/panel_base.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/material/material.hpp"
#include "engine/actor/actor.hpp"

namespace ohao {

/**
 * Dedicated panel for editing MaterialComponent properties
 * Shows PBR material properties, texture management, and material presets
 */
class MaterialComponentPanel : public PanelBase {
public:
    MaterialComponentPanel();
    void render() override;

    void setSelectedActor(Actor* actor) { m_selectedActor = actor; }
    Actor* getSelectedActor() const { return m_selectedActor; }

private:
    void renderMaterialProperties(MaterialComponent* component);
    void renderPBRMaterialProperties(Material& material);

    Actor* m_selectedActor{nullptr};
};

} // namespace ohao
