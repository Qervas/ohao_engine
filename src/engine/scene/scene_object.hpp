#pragma once
#include "engine/asset/model.hpp"
#include "renderer/material/material.hpp"
#include "engine/scene/transform.hpp"
#include <string>
#include <memory>
#include <atomic>

namespace ohao {

// Generate unique IDs for scene objects
using ObjectID = uint64_t;

class SceneObject {
public:
    SceneObject(const std::string& name = "Object");
    virtual ~SceneObject() = default;

    ObjectID getID() const { return objectID; }

    // Name
    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    // Transform accessors
    Transform& getTransform() { return transform; }
    const Transform& getTransform() const { return transform; }

    // Component-style material/model
    void setModel(std::shared_ptr<Model> model) { this->model = model; }
    void setMaterial(const Material& material) { this->material = material; }

    std::shared_ptr<Model> getModel() const { return model; }
    const Material& getMaterial() const { return material; }
    Material& getMaterial() { return material; }

    virtual const char* getTypeName() const { return "SceneObject"; }

protected:
    Material material;
    std::shared_ptr<Model> model;

private:
    ObjectID objectID;
    static std::atomic<ObjectID> nextObjectID;
    std::string name;
    Transform transform;
};

} // namespace ohao
