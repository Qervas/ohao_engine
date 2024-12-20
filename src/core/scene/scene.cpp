#include "core/scene/scene.hpp"
#include "scene/scene_node.hpp"
#include "ui/components/console_widget.hpp"
#include <iostream>

namespace ohao {

Scene::Scene() {
    rootNode = std::make_shared<SceneNode>("Root");
}

bool Scene::loadFromFile(const std::string& filename) {
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
    objects[name] = object;
    // OHAO_LOG_DEBUG("Added object to scene: " + name);
    // OHAO_LOG_DEBUG("Total objects in scene: " + std::to_string(objects.size()));
}

void Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

void Scene::removeObject(const std::string& name) {
    objects.erase(name);
}

void Scene::removeLight(const std::string& name) {
    lights.erase(name);
}


SceneNode::Ptr Scene::getRootNode() { return rootNode; }

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

} // namespace ohao
