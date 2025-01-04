#pragma once
#include "ecs/component.hpp"
#include "core/asset/model.hpp"
#include "core/material/material.hpp"

namespace ohao {

class MeshComponent : public Component {
public:
    void onAttach() override;
    void onDetach() override;

    void setMesh(std::shared_ptr<Model> mesh) { this->mesh = mesh; }
    void setMaterial(std::shared_ptr<Material> material) { this->material = material; }

    std::shared_ptr<Model> getMesh() const { return mesh; }
    std::shared_ptr<Material> getMaterial() const { return material; }

private:
    std::shared_ptr<Model> mesh;
    std::shared_ptr<Material> material;
};

} // namespace ohao
