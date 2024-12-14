#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include "core/asset/model.hpp"
#include "core/scene/scene_node.hpp"
#include "core/scene/scene_object.hpp"
#include "core/material/material.hpp"

namespace ohao {
struct Light {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    bool enabled{true};
};

class Scene {
public:
    Scene();
    ~Scene() = default;

    // Scene management
    bool loadFromFile(const std::string& filename);
    template<typename Func>
    void traverseScene(Func&& callback);

    //adder
    void addObject(const std::string& name, std::shared_ptr<SceneObject> object);
    void addLight(const std::string& name, const Light& light);

    //remover
    void removeObject(const std::string& name);
    void removeLight(const std::string& name);

    //getter
    SceneNode::Ptr getRootNode();
    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>&
                            getObjects() const;
    std::shared_ptr<SceneObject> getObject(const std::string& name);
    const std::unordered_map<std::string, Light>& getLights() const;
    Light* getLight(const std::string& name);

    //setter
    void setRootNode(SceneNode::Ptr node);
    void setObjectMaterial(const std::string& objectName, const Material& material);
    void updateLight(const std::string& name, const Light& light);

    // Material management
    void convertAndAssignMaterial(const std::string& objectName, const MaterialData& mtlData);
    Material convertMTLToMaterial(const MaterialData& mtlData);

private:
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> objects;
    std::unordered_map<std::string, Light> lights;
    SceneNode::Ptr rootNode;

    template <typename Func>
    void traverseNode(SceneNode::Ptr node, Func&& callback);

    void parseModelMaterials(const Model& model);
    void setupDefaultMaterial(Material& material);
};

} // namespace ohao
