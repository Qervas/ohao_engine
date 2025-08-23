#pragma once
#include "engine/asset/model.hpp"
#include "engine/scene/scene_node.hpp"
#include "renderer/material/material.hpp"
#include <string>
#include <memory>
#include <atomic>

namespace ohao {

// Generate unique IDs for scene objects
using ObjectID = uint64_t;

class SceneObject : public SceneNode {
public:
    SceneObject(const std::string& name = "Object");
    ~SceneObject() override = default;

    // Get the unique object ID
    ObjectID getID() const { return objectID; }

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
    
private:
    // Unique ID for each object - never changes after creation
    ObjectID objectID;
    
    // Static counter for generating unique IDs
    static std::atomic<ObjectID> nextObjectID;
};

} // namespace ohao
