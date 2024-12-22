#pragma once
#include "core/asset/model.hpp"
#include "core/scene/scene_node.hpp"
#include "core/material/material.hpp"

namespace ohao {

class SceneObject : public SceneNode {
public:
    SceneObject(const std::string& name = "Object");
    ~SceneObject() override = default;

    // Component accessors
    void setModel(std::shared_ptr<Model> model) { this->model = model; }
    void setMaterial(const Material& material) { this->material = material; }

    std::shared_ptr<Model> getModel() const { return model; }
    const Material& getMaterial() const { return material; }
    Material& getMaterial() { return material; }

    // Type information
    virtual const char* getTypeName() const { return "SceneObject"; }

    // Clone support
    virtual std::shared_ptr<SceneObject> clone() const;
    virtual void setTransform(const Transform& transform) override;
    virtual void markTransformDirty() override;

protected:
    void onAddedToScene() override;
    void onRemovedFromScene() override;

    Material material;
    std::shared_ptr<Model> model;
};

} // namespace ohao
