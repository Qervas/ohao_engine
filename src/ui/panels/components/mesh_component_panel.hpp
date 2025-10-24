#pragma once

#include "ui/common/panel_base.hpp"
#include "renderer/components/mesh_component.hpp"
#include "engine/asset/model.hpp"
#include "engine/actor/actor.hpp"
#include <memory>

namespace ohao {

/**
 * Dedicated panel for editing MeshComponent properties
 * Shows model information, vertex/index counts, and mesh replacement options
 */
class MeshComponentPanel : public PanelBase {
public:
    MeshComponentPanel();
    void render() override;

    void setSelectedActor(Actor* actor) { m_selectedActor = actor; }
    Actor* getSelectedActor() const { return m_selectedActor; }

private:
    void renderMeshProperties(MeshComponent* component);

    // Primitive types for mesh generation
    enum class PrimitiveType {
        Empty,
        Cube,
        Sphere,
        Platform,
        Cylinder,
        Cone
    };

    std::shared_ptr<Model> generatePrimitiveMesh(PrimitiveType type);

    Actor* m_selectedActor{nullptr};
};

} // namespace ohao
