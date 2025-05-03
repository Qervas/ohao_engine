#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include "core/asset/model.hpp"
#include "core/scene/scene_node.hpp"
#include "core/scene/scene_object.hpp"
#include "core/material/material.hpp"
#include "nlohmann/json.hpp"

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
    bool loadModelFromFile(const std::string& filename);
    template<typename Func>
    void traverseScene(Func&& callback);

    //adder
    void addObject(const std::string& name, std::shared_ptr<SceneObject> object);
    void addLight(const std::string& name, const Light& light);

    //remover
    void removeObject(const std::string& name);
    void removeObjectByID(ObjectID id);
    void removeLight(const std::string& name);

    //getter
    SceneNode::Ptr getRootNode();
    const std::unordered_map<std::string, std::shared_ptr<SceneObject>>& getObjectsByName() const;
    const std::unordered_map<ObjectID, std::shared_ptr<SceneObject>>& getObjectsByID() const;
    std::shared_ptr<SceneObject> getObject(const std::string& name);
    std::shared_ptr<SceneObject> getObjectByID(ObjectID id);
    const std::unordered_map<std::string, Light>& getLights() const;
    Light* getLight(const std::string& name);
    const std::string& getProjectPath() const { return projectPath; }
    const std::string& getProjectDir() const { return projectDir; }

    //setter
    void setRootNode(SceneNode::Ptr node);
    void setObjectMaterial(const std::string& objectName, const Material& material);
    void setObjectMaterialByID(ObjectID objectID, const Material& material);
    void updateLight(const std::string& name, const Light& light);
    void setName(const std::string& name);
    void setProjectPath(const std::string& path);

    // Material management
    void convertAndAssignMaterial(const std::string& objectName, const MaterialData& mtlData);
    Material convertMTLToMaterial(const MaterialData& mtlData);

    // serialization
    bool saveToFile(const std::string& filename);
    bool loadFromFile(const std::string& filename);
    static const std::string PROJECT_FILE_EXTENSION; // = ".ohao";
    void validateTransformHierarchy();

private:
    // Two maps for object lookup - by name and by ID
    std::unordered_map<std::string, std::shared_ptr<SceneObject>> objectsByName;
    std::unordered_map<ObjectID, std::shared_ptr<SceneObject>> objectsByID;
    std::unordered_map<std::string, Light> lights;
    SceneNode::Ptr rootNode;
    std::string sceneName;
    std::string projectPath;
    std::string projectDir;

    template <typename Func>
    void traverseNode(SceneNode::Ptr node, Func&& callback);

    void parseModelMaterials(const Model& model);
    void setupDefaultMaterial(Material& material);
    nlohmann::json serializeToJson() const;
    bool deserializeFromJson(const nlohmann::json& json);
    void validateNodeTransforms(SceneNode* node);
};

} // namespace ohao
