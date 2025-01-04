#include "mesh_component.hpp"
#include "ecs/entity.hpp"
#include "ui/components/console_widget.hpp"
namespace ohao {

void MeshComponent::onAttach() {
    OHAO_LOG_DEBUG("MeshComponent attached to entity: " +
                   (getOwner() ? getOwner()->getName() : "Unknown"));
}

void MeshComponent::onDetach() {
    OHAO_LOG_DEBUG("MeshComponent detached from entity: " +
                   (getOwner() ? getOwner()->getName() : "Unknown"));
    mesh.reset();
    material.reset();
}

} // namespace ohao
