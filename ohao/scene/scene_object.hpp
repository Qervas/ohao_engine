#pragma once
#include "scene/asset/model.hpp"
#include "gpu/vulkan/material.hpp"
#include "scene/transform.hpp"
#include <string>
#include <string_view>
#include <memory>
#include <atomic>

namespace ohao {

// Generate unique IDs for scene objects
using ObjectID = uint64_t;

class SceneObject {
public:
    explicit SceneObject(std::string_view name = "Object");
    virtual ~SceneObject() = default;

    [[nodiscard]] ObjectID getID() const { return objectID; }

    // Name
    [[nodiscard]] const std::string& getName() const { return name; }
    void setName(std::string_view n) { name = std::string(n); }

    // Transform accessors
    [[nodiscard]] Transform& getTransform() { return transform; }
    [[nodiscard]] const Transform& getTransform() const { return transform; }

    // Component-style material/model
    void setModel(std::shared_ptr<Model> model) { this->model = std::move(model); }
    void setMaterial(const Material& material) { this->material = material; }

    [[nodiscard]] std::shared_ptr<Model> getModel() const { return model; }
    [[nodiscard]] const Material& getMaterial() const { return material; }
    [[nodiscard]] Material& getMaterial() { return material; }

    [[nodiscard]] virtual const char* getTypeName() const { return "SceneObject"; }

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
