#pragma once
#include "model.hpp"
#include "material.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>

namespace ohao {
struct Light {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    bool enabled{true};
};

class SceneObject {
public:
    SceneObject(const std::string& name = "Object") : name(name) {}

    std::string name;
    glm::mat4 transform{1.0f};
    Material material;
    std::shared_ptr<Model> model;
    bool visible{true};
};

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    // Scene management
    void loadFromFile(const std::string& filename);
    void addObject(const std::string& name, std::shared_ptr<SceneObject> object);
    void removeObject(const std::string& name);

    // Light management
    void addLight(const std::string& name, const Light& light);
    void removeLight(const std::string& name);
    void updateLight(const std::string& name, const Light& light);

    // Material management
    void convertAndAssignMaterial(const std::string& objectName, const MaterialData& mtlData);
    Material convertMTLToMaterial(const MaterialData& mtlData);
    void setObjectMaterial(const std::string& objectName, const Material& material);

    // Getters
    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>& getObjects() const { return objects; }
    const std::unordered_map<std::string, Light>& getLights() const { return lights; }
    std::shared_ptr<SceneObject> getObject(const std::string& name);
    Light* getLight(const std::string& name);

private:
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> objects;
    std::unordered_map<std::string, Light> lights;

    void parseModelMaterials(const Model& model);
    void setupDefaultMaterial(Material& material);
};

} // namespace ohao
