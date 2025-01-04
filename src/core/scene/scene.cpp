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

        std::cout << "Scene setup complete. Objects in scene: " << objects.size() << std::endl;
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
    // Add to objects map
    objects[name] = object;
    OHAO_LOG_DEBUG("Added object to scene: " + name);
}

void Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

void Scene::removeObject(const std::string& name) {
    auto it = objects.find(name);
    if (it == objects.end()) {
        OHAO_LOG_WARNING("Attempt to remove non-existent object: " + name);
        return;
    }

    try {
        // Get shared_ptr to object before removing from map
        auto object = it->second;

        // Remove from map first
        objects.erase(it);

        // Then safely detach from hierarchy
        if (object) {
            if (object->getParent()) {
                object->detachFromParent();
            }
        }

        OHAO_LOG_DEBUG("Successfully removed object: " + name);

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error removing object: " + std::string(e.what()));
    }
}

void Scene::removeLight(const std::string& name) {
    lights.erase(name);
}



const std::unordered_map<std::string, std::shared_ptr<SceneObject>>&
Scene::getObjects() const{
    return objects;
}

std::shared_ptr<SceneObject>
Scene::getObject(const std::string& name) {
    auto it = objects.find(name);
    return (it != objects.end()) ? it->second : nullptr;
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
    if (auto it = objects.find(objectName); it != objects.end()) {
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
    nlohmann::json j;

    // Save scene metadata
    j["name"] = sceneName;
    j["version"] = "1.0";

    // Save objects
    j["objects"] = nlohmann::json::array();
    for (const auto& [name, obj] : objects) {
        nlohmann::json objData;
        objData["name"] = obj->getName();

        // Save transform
        objData["transform"] = {
            {"position", {
                obj->getTransform().getLocalPosition().x,
                obj->getTransform().getLocalPosition().y,
                obj->getTransform().getLocalPosition().z
            }},
            {"rotation", {
                glm::eulerAngles(obj->getTransform().getLocalRotation()).x,
                glm::eulerAngles(obj->getTransform().getLocalRotation()).y,
                glm::eulerAngles(obj->getTransform().getLocalRotation()).z
            }},
            {"scale", {
                obj->getTransform().getLocalScale().x,
                obj->getTransform().getLocalScale().y,
                obj->getTransform().getLocalScale().z
            }}
        };

        // Save material
        const auto& material = obj->getMaterial();
        objData["material"] = {
            {"baseColor", {material.baseColor.x, material.baseColor.y, material.baseColor.z}},
            {"metallic", material.metallic},
            {"roughness", material.roughness},
            {"ao", material.ao},
            {"ior", material.ior},
            {"emissive", {material.emissive.x, material.emissive.y, material.emissive.z}}
        };

        // Save model path if exists
        if (obj->getModel()) {
            std::string modelPath = obj->getModel()->getSourcePath();

            // Convert to relative path if possible
            if (!modelPath.empty()) {
                try {
                    // If we have a project path, make the model path relative to it
                    if (!projectPath.empty()) {
                        std::filesystem::path fullPath = std::filesystem::absolute(modelPath);
                        std::filesystem::path projPath = std::filesystem::absolute(projectPath);

                        // Try to make it relative to project path
                        if (auto relPath = std::filesystem::relative(fullPath, projPath.parent_path());
                            !relPath.empty()) {
                            modelPath = relPath.string();
                        }
                    }

                    objData["modelPath"] = modelPath;

                    // Optionally save a hash or timestamp for file change detection
                    auto lastWriteTime = std::filesystem::last_write_time(modelPath);
                    auto timeStamp = std::chrono::duration_cast<std::chrono::seconds>(
                        lastWriteTime.time_since_epoch()).count();
                    objData["modelTimeStamp"] = timeStamp;

                } catch (const std::filesystem::filesystem_error& e) {
                    OHAO_LOG_WARNING("Failed to process model path: " + std::string(e.what()));
                    objData["modelPath"] = modelPath;  // Save absolute path as fallback
                }
            }
        }

        j["objects"].push_back(objData);
    }

    // Save lights
    j["lights"] = nlohmann::json::array();
    for (const auto& [name, light] : lights) {
        nlohmann::json lightData;
        lightData["name"] = name;
        lightData["position"] = {light.position.x, light.position.y, light.position.z};
        lightData["color"] = {light.color.x, light.color.y, light.color.z};
        lightData["intensity"] = light.intensity;
        lightData["enabled"] = light.enabled;
        j["lights"].push_back(lightData);
    }

    return j;
}

bool Scene::deserializeFromJson(const nlohmann::json& j) {
    try {
        // Load scene metadata
        sceneName = j["name"].get<std::string>();
        std::string version = j["version"].get<std::string>();
        OHAO_LOG_DEBUG("Loading scene: " + sceneName + " (version " + version + ")");

        // Clear existing objects
        objects.clear();
        rootNode = std::make_shared<SceneNode>("Root");

        // Load objects
        for (const auto& objData : j["objects"]) {
            try {
                // Create new object
                auto obj = std::make_shared<SceneObject>(objData["name"].get<std::string>());

                // Load transform
                auto& transformData = objData["transform"];
                auto& position = transformData["position"];
                obj->getTransform().setLocalPosition(glm::vec3(
                    position[0].get<float>(),
                    position[1].get<float>(),
                    position[2].get<float>()
                ));

                // Load rotation if present
                if (transformData.contains("rotation")) {
                    auto& rotation = transformData["rotation"];
                    obj->getTransform().setLocalRotationEuler(glm::vec3(
                        rotation[0].get<float>(),
                        rotation[1].get<float>(),
                        rotation[2].get<float>()
                    ));
                }

                // Load scale if present
                if (transformData.contains("scale")) {
                    auto& scale = transformData["scale"];
                    obj->getTransform().setLocalScale(glm::vec3(
                        scale[0].get<float>(),
                        scale[1].get<float>(),
                        scale[2].get<float>()
                    ));
                }

                // Load material if present
                if (objData.contains("material")) {
                    auto& matData = objData["material"];
                    Material material;
                    material.baseColor = glm::vec3(
                        matData["baseColor"][0].get<float>(),
                        matData["baseColor"][1].get<float>(),
                        matData["baseColor"][2].get<float>()
                    );
                    material.metallic = matData["metallic"].get<float>();
                    material.roughness = matData["roughness"].get<float>();
                    material.ao = matData["ao"].get<float>();
                    material.ior = matData["ior"].get<float>();

                    if (matData.contains("emissive")) {
                        material.emissive = glm::vec3(
                            matData["emissive"][0].get<float>(),
                            matData["emissive"][1].get<float>(),
                            matData["emissive"][2].get<float>()
                        );
                    }

                    obj->setMaterial(material);
                }

                // Load model data if present
                if (objData.contains("modelPath")) {
                    std::string modelPath = objData["modelPath"].get<std::string>();
                    if (!modelPath.empty()) {
                        // If path is relative, make it absolute using project directory
                        std::filesystem::path fullPath = modelPath;
                        if (fullPath.is_relative() && !projectDir.empty()) {
                            fullPath = std::filesystem::path(projectDir) / modelPath;
                        }

                        auto model = std::make_shared<Model>();
                        if (model->loadFromOBJ(fullPath.string())) {
                            obj->setModel(model);

                            // Verify timestamp if available
                            if (objData.contains("modelTimeStamp")) {
                                auto savedTime = objData["modelTimeStamp"].get<int64_t>();
                                auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::filesystem::last_write_time(fullPath).time_since_epoch()).count();

                                if (savedTime != currentTime) {
                                    OHAO_LOG_WARNING("Model file has been modified since last save: " + modelPath);
                                }
                            }
                        } else {
                            OHAO_LOG_WARNING("Failed to load model: " + fullPath.string());
                        }
                    }
                }

                // Add to scene
                rootNode->addChild(obj);
                objects[obj->getName()] = obj;

                OHAO_LOG_DEBUG("Loaded object: " + obj->getName());

            } catch (const std::exception& e) {
                OHAO_LOG_ERROR("Failed to load object: " + std::string(e.what()));
                continue;
            }
        }

        // Load lights if present
        if (j.contains("lights")) {
            for (const auto& lightData : j["lights"]) {
                try {
                    Light light;
                    auto& position = lightData["position"];
                    light.position = glm::vec3(
                        position[0].get<float>(),
                        position[1].get<float>(),
                        position[2].get<float>()
                    );

                    auto& color = lightData["color"];
                    light.color = glm::vec3(
                        color[0].get<float>(),
                        color[1].get<float>(),
                        color[2].get<float>()
                    );

                    light.intensity = lightData["intensity"].get<float>();
                    light.enabled = lightData["enabled"].get<bool>();

                    std::string lightName = lightData["name"].get<std::string>();
                    lights[lightName] = light;

                    OHAO_LOG_DEBUG("Loaded light: " + lightName);

                } catch (const std::exception& e) {
                    OHAO_LOG_ERROR("Failed to load light: " + std::string(e.what()));
                    continue;
                }
            }
        }

        OHAO_LOG_DEBUG("Scene loaded successfully");
        return true;

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to deserialize scene: " + std::string(e.what()));
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

} // namespace ohao
