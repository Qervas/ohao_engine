#include "scene_serializer.hpp"
#include "scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/light_component.hpp"
#include "renderer/components/material_component.hpp"
#include "physics/components/physics_component.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace ohao {

std::string SceneSerializer::s_lastError;

// === Helper: primitive type to/from string ===

static std::string primitiveToString(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Cube: return "cube";
        case PrimitiveType::Sphere: return "sphere";
        case PrimitiveType::Platform: return "platform";
        case PrimitiveType::Cylinder: return "cylinder";
        case PrimitiveType::DirectionalLight: return "directional_light";
        case PrimitiveType::PointLight: return "point_light";
        default: return "unknown";
    }
}

static PrimitiveType stringToPrimitive(const std::string& str) {
    if (str == "cube") return PrimitiveType::Cube;
    if (str == "sphere") return PrimitiveType::Sphere;
    if (str == "platform") return PrimitiveType::Platform;
    if (str == "cylinder") return PrimitiveType::Cylinder;
    if (str == "directional_light") return PrimitiveType::DirectionalLight;
    if (str == "point_light") return PrimitiveType::PointLight;
    return PrimitiveType::Cube;
}

// === Serialize ===

nlohmann::json SceneSerializer::serialize(const Scene* scene) {
    nlohmann::json root;
    root["name"] = scene->getName();
    root["version"] = "1.0";

    nlohmann::json actors_array = nlohmann::json::array();

    for (const auto& [id, actor] : scene->getAllActors()) {
        nlohmann::json actorJson;
        actorJson["name"] = actor->getName();
        actorJson["id"] = id;

        // Transform
        auto transform = actor->getTransform();
        if (transform) {
            glm::vec3 pos = transform->getPosition();
            glm::quat rot = transform->getRotation();
            glm::vec3 scale = transform->getScale();

            actorJson["transform"] = {
                {"position", {pos.x, pos.y, pos.z}},
                {"rotation", {rot.x, rot.y, rot.z, rot.w}},
                {"scale", {scale.x, scale.y, scale.z}}
            };
        }

        // Mesh presence (primitive type inferred from actor name or defaults to cube)
        auto mesh = actor->getComponent<MeshComponent>();
        if (mesh) {
            // Infer primitive type from actor name convention
            std::string lowerName = actor->getName();
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            std::string primStr = "cube";
            if (lowerName.find("sphere") != std::string::npos) primStr = "sphere";
            else if (lowerName.find("plane") != std::string::npos || lowerName.find("platform") != std::string::npos || lowerName.find("floor") != std::string::npos || lowerName.find("ground") != std::string::npos) primStr = "platform";
            else if (lowerName.find("cylinder") != std::string::npos) primStr = "cylinder";
            actorJson["mesh"] = {{"primitive", primStr}};
        }

        // Material
        auto material = actor->getComponent<MaterialComponent>();
        if (material) {
            const auto& mat = material->getMaterial();
            actorJson["material"] = {
                {"baseColor", {mat.baseColor.r, mat.baseColor.g, mat.baseColor.b}},
                {"metallic", mat.metallic},
                {"roughness", mat.roughness}
            };
        }

        // Light
        auto light = actor->getComponent<LightComponent>();
        if (light) {
            std::string lightType;
            switch (light->getLightType()) {
                case LightType::Directional: lightType = "directional"; break;
                case LightType::Point: lightType = "point"; break;
                case LightType::Spot: lightType = "spot"; break;
                default: lightType = "point"; break;
            }

            glm::vec3 color = light->getColor();
            glm::vec3 dir = light->getDirection();
            nlohmann::json lightJson = {
                {"type", lightType},
                {"color", {color.r, color.g, color.b}},
                {"intensity", light->getIntensity()},
                {"range", light->getRange()}
            };
            if (lightType == "directional") {
                lightJson["direction"] = {dir.x, dir.y, dir.z};
            }
            actorJson["light"] = lightJson;
        }

        // Physics
        auto physics = actor->getComponent<PhysicsComponent>();
        if (physics) {
            std::string bodyType;
            switch (physics->getRigidBodyType()) {
                case physics::dynamics::RigidBodyType::DYNAMIC: bodyType = "dynamic"; break;
                case physics::dynamics::RigidBodyType::STATIC: bodyType = "static"; break;
                case physics::dynamics::RigidBodyType::KINEMATIC: bodyType = "kinematic"; break;
                default: bodyType = "dynamic"; break;
            }
            actorJson["physics"] = {
                {"bodyType", bodyType},
                {"mass", physics->getMass()},
                {"friction", physics->getFriction()},
                {"restitution", physics->getRestitution()}
            };
        }

        actors_array.push_back(actorJson);
    }

    root["actors"] = actors_array;
    return root;
}

// === Deserialize ===

bool SceneSerializer::deserialize(Scene* scene, const nlohmann::json& json) {
    try {
        if (!json.contains("actors")) {
            s_lastError = "JSON missing 'actors' array";
            return false;
        }

        // Clear existing scene
        scene->removeAllActors();

        if (json.contains("name")) {
            scene->setName(json["name"].get<std::string>());
        }

        for (const auto& actorJson : json["actors"]) {
            std::string name = actorJson.value("name", "Actor");

            // Determine primitive type from mesh or light data
            PrimitiveType primType = PrimitiveType::Cube;
            if (actorJson.contains("mesh")) {
                primType = stringToPrimitive(actorJson["mesh"].value("primitive", "cube"));
            } else if (actorJson.contains("light")) {
                std::string lightType = actorJson["light"].value("type", "point");
                if (lightType == "directional") primType = PrimitiveType::DirectionalLight;
                else primType = PrimitiveType::PointLight;
            }

            auto actor = scene->createActorWithComponents(name, primType);
            if (!actor) continue;

            // Transform
            if (actorJson.contains("transform")) {
                auto transform = actor->getTransform();
                if (transform) {
                    const auto& t = actorJson["transform"];
                    if (t.contains("position")) {
                        auto& p = t["position"];
                        transform->setPosition(glm::vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>()));
                    }
                    if (t.contains("rotation")) {
                        auto& r = t["rotation"];
                        transform->setRotation(glm::quat(r[3].get<float>(), r[0].get<float>(), r[1].get<float>(), r[2].get<float>()));
                    }
                    if (t.contains("scale")) {
                        auto& s = t["scale"];
                        transform->setScale(glm::vec3(s[0].get<float>(), s[1].get<float>(), s[2].get<float>()));
                    }
                }
            }

            // Material
            if (actorJson.contains("material")) {
                auto material = actor->getComponent<MaterialComponent>();
                if (material) {
                    const auto& m = actorJson["material"];
                    if (m.contains("baseColor")) {
                        auto& c = m["baseColor"];
                        material->getMaterial().baseColor = glm::vec3(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
                    }
                    if (m.contains("metallic")) {
                        material->getMaterial().metallic = m["metallic"].get<float>();
                    }
                    if (m.contains("roughness")) {
                        material->getMaterial().roughness = m["roughness"].get<float>();
                    }
                }
            }

            // Light
            if (actorJson.contains("light")) {
                auto light = actor->getComponent<LightComponent>();
                if (light) {
                    const auto& l = actorJson["light"];
                    if (l.contains("color")) {
                        auto& c = l["color"];
                        light->setColor(glm::vec3(c[0].get<float>(), c[1].get<float>(), c[2].get<float>()));
                    }
                    if (l.contains("intensity")) {
                        light->setIntensity(l["intensity"].get<float>());
                    }
                    if (l.contains("range")) {
                        light->setRange(l["range"].get<float>());
                    }
                    if (l.contains("direction")) {
                        auto& d = l["direction"];
                        light->setDirection(glm::normalize(glm::vec3(d[0].get<float>(), d[1].get<float>(), d[2].get<float>())));
                    }
                }
            }
        }

        return true;
    } catch (const std::exception& e) {
        s_lastError = std::string("Deserialization error: ") + e.what();
        return false;
    }
}

// === File I/O ===

bool SceneSerializer::saveToFile(const Scene* scene, const std::string& filepath) {
    try {
        nlohmann::json json = serialize(scene);
        std::ofstream file(filepath);
        if (!file.is_open()) {
            s_lastError = "Failed to open file for writing: " + filepath;
            return false;
        }
        file << json.dump(2);
        return true;
    } catch (const std::exception& e) {
        s_lastError = std::string("Save error: ") + e.what();
        return false;
    }
}

bool SceneSerializer::loadFromFile(Scene* scene, const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            s_lastError = "Failed to open file for reading: " + filepath;
            return false;
        }
        nlohmann::json json = nlohmann::json::parse(file);
        return deserialize(scene, json);
    } catch (const std::exception& e) {
        s_lastError = std::string("Load error: ") + e.what();
        return false;
    }
}

const std::string& SceneSerializer::getLastError() {
    return s_lastError;
}

} // namespace ohao
