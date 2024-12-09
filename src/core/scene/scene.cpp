#include "scene.hpp"
#include <iostream>

namespace ohao {

void
Scene::loadFromFile(const std::string& filename) {
    auto sceneObject = std::make_shared<SceneObject>("cornell_box");
    sceneObject->model = std::make_shared<Model>();
    sceneObject->model->loadFromOBJ(filename);

    // Parse materials and lights from the model
    parseModelMaterials(*sceneObject->model);

    objects["cornell_box"] = sceneObject;
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
        object->material = convertMTLToMaterial(mtlData);
        addObject(name, object);
    }
}

Material
Scene::convertMTLToMaterial(const MaterialData& mtlData) {
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

void
Scene::setupDefaultMaterial(Material& material) {
    material.baseColor = glm::vec3(0.8f);
    material.metallic = 0.0f;
    material.roughness = 0.5f;
    material.ao = 1.0f;
    material.emissive = glm::vec3(0.0f);
    material.ior = 1.45f;
}

void
Scene::addObject(const std::string& name, std::shared_ptr<SceneObject> object) {
    objects[name] = object;
}

std::shared_ptr<SceneObject>
Scene::getObject(const std::string& name) {
    auto it = objects.find(name);
    return (it != objects.end()) ? it->second : nullptr;
}

void
Scene::removeObject(const std::string& name) {
    objects.erase(name);
}

void
Scene::addLight(const std::string& name, const Light& light) {
    lights[name] = light;
}

Light*
Scene::getLight(const std::string& name) {
    auto it = lights.find(name);
    return (it != lights.end()) ? &it->second : nullptr;
}

void
Scene::removeLight(const std::string& name) {
    lights.erase(name);
}

void
Scene::updateLight(const std::string& name, const Light& light) {
    if (lights.find(name) != lights.end()) {
        lights[name] = light;
    }
}

void
Scene::setObjectMaterial(const std::string& objectName, const Material& material) {
    if (auto it = objects.find(objectName); it != objects.end()) {
        it->second->material = material;
    }
}

} // namespace ohao
