#include "core/scene/scene.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene_node.hpp"
#include "ui/components/console_widget.hpp"
#include "ui/selection/selection_manager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>

namespace ohao {

Scene::Scene() {
    rootNode = std::make_shared<SceneNode>("Root");
}

SceneNode::Ptr Scene::getRootNode() { return rootNode; }


bool Scene::loadModelFromFile(const std::string& filename) {
    try {
        auto sceneObject = std::make_shared<SceneObject>("cornell_box");
        sceneObject->setModel(std::make_shared<Model>());

        std::cout << "Attempting to load model from: " << filename << std::endl;

        if (!sceneObject->getModel()->loadFromOBJ(filename)) {
            std::cerr << "Failed to load OBJ file: " << filename << std::endl;
            return false;
        }

        std::cout << "OBJ file loaded successfully, parsing materials..." << std::endl;
        parseModelMaterials(*sceneObject->getModel());

        getRootNode()->addChild(sceneObject);
        addObject(sceneObject->getName(), sceneObject);  // Make sure to add to objects map

        std::cout << "Scene setup complete. Objects in scene: " << objectsByName.size() << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Exception while loading scene: " << e.what() << std::endl;
        return false;
    }
}

void Scene::parseModelMaterials(const Model& model) {
    for (const auto& [name, mtlData] : model.materials) {
        if (mtlData.isLight) {
            Light light;
            light.position = mtlData.lightPosition.length() > 0 ?
                           mtlData.lightPosition :
                           glm::vec3(0.0f, 0.9f, 0.0f); // Default position
            light.color = mtlData.emission;
            light.intensity = mtlData.lightIntensity > 0 ?
                           mtlData.lightIntensity :
                           glm::length(mtlData.emission); // Use emission magnitude as fallback
            addLight(name, light);

            std::cout << "Added light: " << name
                     << " pos: " << light.position.x << ","
                     << light.position.y << ","
                     << light.position.z
                     << " intensity: " << light.intensity
                     << " emission: " << mtlData.emission.x << ","
                     << mtlData.emission.y << ","
                     << mtlData.emission.z << std::endl;
        }

        // Convert MTL material to our PBR material
        auto object = std::make_shared<SceneObject>(name);
        object->setMaterial(convertMTLToMaterial(mtlData));
        addObject(name, object);
    }
}

Material Scene::convertMTLToMaterial(const MaterialData& mtlData) {
    Material material;

    // Convert traditional material properties to PBR parameters
    if (mtlData.isLight) {
        material.baseColor = mtlData.emission;
        material.emissive = mtlData.emission;
    } else {
        material.baseColor = mtlData.diffuse;
        material.emissive = glm::vec3(0.0f);
    }

    // Calculate metallic value from specular intensity
    float specIntensity = (mtlData.specular.r + mtlData.specular.g + mtlData.specular.b) / 3.0f;
    material.metallic = specIntensity;

    // Convert shininess to roughness (inverse relationship)
    material.roughness = 1.0f - glm::clamp(mtlData.shininess / 100.0f, 0.0f, 1.0f);

    // Set ambient occlusion from ambient term
    material.ao = (mtlData.ambient.r + mtlData.ambient.g + mtlData.ambient.b) / 3.0f;
    material.ior = mtlData.ior;

    return material;
}

void Scene::setupDefaultMaterial(Material& material) {
    material.baseColor = glm::vec3(0.8f);
    material.metallic = 0.0f;
    material.roughness = 0.5f;
    material.ao = 1.0f;
    material.emissive = glm::vec3(0.0f);
    material.ior = 1.45f;
}

template<typename Func>
void Scene::traverseScene(Func&& callback) {
    traverseNode(rootNode.get(), std::forward<Func>(callback));
}

void Scene::addObject(const std::string& name, std::shared_ptr<SceneObject> object) {
    if (!object) return;
    
    // Add to both lookup maps
    objectsByName[name] = object;
    objectsByID[object->getID()] = object;
    
    OHAO_LOG_DEBUG("Added object to scene: " + name + " (ID: " + std::to_string(object->getID()) + ")");
}

void Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

void Scene::removeObject(const std::string& name) {
    auto it = objectsByName.find(name);
    if (it == objectsByName.end()) {
        OHAO_LOG_WARNING("Attempt to remove non-existent object: " + name);
        return;
    }

    try {
        // Get shared_ptr to object before removing from map
        auto object = it->second;
        ObjectID id = object->getID();

        // Remove from both maps
        objectsByName.erase(it);
        objectsByID.erase(id);

        // Then safely detach from hierarchy
        if (object) {
            if (object->getParent()) {
                object->detachFromParent();
            }
        }

        OHAO_LOG_DEBUG("Successfully removed object: " + name + " (ID: " + std::to_string(id) + ")");

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error removing object: " + std::string(e.what()));
    }
}

void Scene::removeObjectByID(ObjectID id) {
    auto it = objectsByID.find(id);
    if (it == objectsByID.end()) {
        OHAO_LOG_WARNING("Attempt to remove non-existent object ID: " + std::to_string(id));
        return;
    }

    try {
        // Get shared_ptr to object before removing from map
        auto object = it->second;
        std::string name = object->getName();

        // Remove from both maps
        objectsByID.erase(id);
        objectsByName.erase(name);

        // Then safely detach from hierarchy
        if (object) {
            if (object->getParent()) {
                object->detachFromParent();
            }
        }

        OHAO_LOG_DEBUG("Successfully removed object: " + name + " (ID: " + std::to_string(id) + ")");

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error removing object: " + std::string(e.what()));
    }
}

void Scene::removeLight(const std::string& name) {
    lights.erase(name);
}

const std::unordered_map<std::string, std::shared_ptr<SceneObject>>&
Scene::getObjectsByName() const {
    return objectsByName;
}

const std::unordered_map<ObjectID, std::shared_ptr<SceneObject>>&
Scene::getObjectsByID() const {
    return objectsByID;
}

std::shared_ptr<SceneObject>
Scene::getObject(const std::string& name) {
    auto it = objectsByName.find(name);
    return (it != objectsByName.end()) ? it->second : nullptr;
}

std::shared_ptr<SceneObject>
Scene::getObjectByID(ObjectID id) {
    auto it = objectsByID.find(id);
    return (it != objectsByID.end()) ? it->second : nullptr;
}

const std::unordered_map<std::string, Light>&
Scene::getLights() const {
    return lights;
}

Light* Scene::getLight(const std::string& name) {
    auto it = lights.find(name);
    return (it != lights.end()) ? &it->second : nullptr;
}

void Scene::updateLight(const std::string& name, const Light& light) {
    if (lights.find(name) != lights.end()) {
        lights[name] = light;
    }
}

void Scene::setObjectMaterial(const std::string& objectName, const Material& material) {
    if (auto it = objectsByName.find(objectName); it != objectsByName.end()) {
        it->second->setMaterial(material);
    }
}

void Scene::setObjectMaterialByID(ObjectID objectID, const Material& material) {
    if (auto it = objectsByID.find(objectID); it != objectsByID.end()) {
        it->second->setMaterial(material);
    }
}

void Scene::setRootNode(SceneNode::Ptr node) {
    rootNode = node;
}

template <typename Func>
void Scene::traverseNode(SceneNode::Ptr node, Func&& callback) {
    if(!node) return;
    callback(node);

    for (auto& child : node->getChildren()) {
        traverseNode(child, callback);
    }
}

void Scene::setName(const std::string& name) {
    sceneName = name;
}

bool Scene::saveToFile(const std::string& filename) {
    try {
        nlohmann::json j = serializeToJson();

        std::basic_ofstream<char> file;
        file.open(filename.c_str(), std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            OHAO_LOG_ERROR("Failed to open file for writing: " + filename);
            return false;
        }
        std::string jsonStr = j.dump(4);
        file.write(jsonStr.c_str(), jsonStr.length());
        file.close();

        if (file.fail()) {
            OHAO_LOG_ERROR("Failed to write to file: " + filename);
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to save scene: " + std::string(e.what()));
        return false;
    }
}

bool Scene::loadFromFile(const std::string& filename) {
    try {
        // Create input file stream
        std::basic_ifstream<char> file;
        file.open(filename, std::ios::in | std::ios::binary);

        if (!file.is_open()) {
            OHAO_LOG_ERROR("Failed to open file for reading: " + filename);
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        nlohmann::json j = nlohmann::json::parse(buffer.str());
        return deserializeFromJson(j);
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to load scene: " + std::string(e.what()));
        return false;
    }
}

nlohmann::json Scene::serializeToJson() const {
    nlohmann::json json;

    // Serialize basic scene info
    json["name"] = sceneName;
    json["projectPath"] = projectPath;

    // Serialize scene objects
    json["objects"] = nlohmann::json::array();
    for (const auto& [name, obj] : objectsByName) {
        nlohmann::json objectJson;
        objectJson["name"] = obj->getName();
        objectJson["id"] = obj->getID();
        
        // Serialize transform
        const auto& transform = obj->getTransform();
        auto pos = transform.getLocalPosition();
        auto rot = transform.getLocalRotation();
        auto scale = transform.getLocalScale();
        
        objectJson["transform"] = {
            {"position", {pos.x, pos.y, pos.z}},
            {"rotation", {rot.w, rot.x, rot.y, rot.z}},
            {"scale", {scale.x, scale.y, scale.z}}
        };

        // Serialize material 
        const auto& material = obj->getMaterial();
        objectJson["material"] = {
            {"baseColor", {material.baseColor.r, material.baseColor.g, material.baseColor.b}},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"ao", material.ao},
            {"emissive", {material.emissive.r, material.emissive.g, material.emissive.b}},
            {"ior", material.ior}
        };

        json["objects"].push_back(objectJson);
    }

    // Serialize lights
    json["lights"] = nlohmann::json::array();
    for (const auto& [name, light] : lights) {
        nlohmann::json lightJson;
        lightJson["name"] = name;
        lightJson["position"] = {light.position.x, light.position.y, light.position.z};
        lightJson["color"] = {light.color.r, light.color.g, light.color.b};
        lightJson["intensity"] = light.intensity;
        lightJson["enabled"] = light.enabled;
        
        json["lights"].push_back(lightJson);
    }

    return json;
}

bool Scene::deserializeFromJson(const nlohmann::json& json) {
    try {
        // Clear existing scene data
        objectsByName.clear();
        objectsByID.clear();
        lights.clear();
        rootNode = std::make_shared<SceneNode>("Root");
        
        // Load basic scene info
        sceneName = json["name"];
        projectPath = json["projectPath"];
        
        // Load objects
        for (const auto& objJson : json["objects"]) {
            auto obj = std::make_shared<SceneObject>(objJson["name"]);
            
            // Load transform
            Transform transform;
            auto& posJson = objJson["transform"]["position"];
            auto& rotJson = objJson["transform"]["rotation"];
            auto& scaleJson = objJson["transform"]["scale"];
            
            glm::vec3 position(posJson[0], posJson[1], posJson[2]);
            glm::quat rotation(rotJson[0], rotJson[1], rotJson[2], rotJson[3]);
            glm::vec3 scale(scaleJson[0], scaleJson[1], scaleJson[2]);
            
            transform.setLocalPosition(position);
            transform.setLocalRotation(rotation);
            transform.setLocalScale(scale);
            obj->setTransform(transform);
            
            // Load material
            Material material;
            auto& matJson = objJson["material"];
            auto& baseColorJson = matJson["baseColor"];
            auto& emissiveJson = matJson["emissive"];
            
            material.baseColor = glm::vec3(baseColorJson[0], baseColorJson[1], baseColorJson[2]);
            material.metallic = matJson["metallic"];
            material.roughness = matJson["roughness"];
            material.ao = matJson["ao"];
            material.emissive = glm::vec3(emissiveJson[0], emissiveJson[1], emissiveJson[2]);
            material.ior = matJson["ior"];
            
            obj->setMaterial(material);
            
            // Add to scene
            rootNode->addChild(obj);
            addObject(obj->getName(), obj);
        }
        
        // Load lights
        for (const auto& lightJson : json["lights"]) {
            Light light;
            auto& posJson = lightJson["position"];
            auto& colorJson = lightJson["color"];
            
            light.position = glm::vec3(posJson[0], posJson[1], posJson[2]);
            light.color = glm::vec3(colorJson[0], colorJson[1], colorJson[2]);
            light.intensity = lightJson["intensity"];
            light.enabled = lightJson["enabled"];
            
            lights[lightJson["name"]] = light;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error deserializing scene: " << e.what() << std::endl;
        return false;
    }
}

void Scene::setProjectPath(const std::string& path) {
    projectPath = path;

    // Extract project directory
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        projectDir = path.substr(0, lastSlash + 1);
    } else {
        projectDir = "";
    }
}

void Scene::validateTransformHierarchy() {
    if (!rootNode) return;
    validateNodeTransforms(rootNode.get());
}

void Scene::validateNodeTransforms(SceneNode* node) {
    if (!node) return;

    // Ensure transform owner is set correctly
    node->getTransform().setOwner(node);

    // Mark dirty to force update
    node->markTransformDirty();

    // Recurse through children
    for (const auto& child : node->getChildren()) {
        validateNodeTransforms(child.get());
    }
}

} // namespace ohao
