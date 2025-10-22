#include "actor_serializer.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/asset/model.hpp"
#include "engine/asset/primitive_mesh_generator.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace ohao {

// Simple enum for primitive types if it doesn't exist elsewhere
enum class PrimitiveType {
    Cube,
    Sphere,
    Platform,
    Cylinder,
    Cone
};

// Helper function to generate primitive meshes
std::shared_ptr<Model> generatePrimitiveMesh(PrimitiveType type) {
    auto model = std::make_shared<Model>();
    
    // TODO: PrimitiveMeshGenerator methods not implemented yet
    /*
    switch (type) {
        case PrimitiveType::Cube:
            PrimitiveMeshGenerator::generateCube(*model);
            break;
        case PrimitiveType::Sphere:
            PrimitiveMeshGenerator::generateSphere(*model);
            break;
        case PrimitiveType::Platform:
            PrimitiveMeshGenerator::generatePlatform(*model);
            break;
        case PrimitiveType::Cylinder:
            PrimitiveMeshGenerator::generateCylinder(*model);
            break;
        case PrimitiveType::Cone:
            PrimitiveMeshGenerator::generateCone(*model);
            break;
    }
    */
    
    return model;
}

nlohmann::json ActorSerializer::serializeActor(const Actor* actor) {
    if (!actor) return nlohmann::json();
    
    nlohmann::json actorJson;
    
    // Serialize basic properties
    actorJson["id"] = actor->getID();
    actorJson["name"] = actor->getName();
    actorJson["active"] = actor->isActive();
    
    // Serialize parent-child relationship
    actorJson["parentId"] = actor->getParent() ? actor->getParent()->getID() : 0;
    
    // Serialize transform component
    auto transform = actor->getTransform();
    if (transform) {
        actorJson["transform"] = serializeTransformComponent(transform);
    }
    
    // Serialize components
    nlohmann::json componentsJson = nlohmann::json::array();
    
    // Check for mesh component
    auto meshComponent = actor->getComponent<MeshComponent>();
    if (meshComponent) {
        nlohmann::json componentJson;
        componentJson["type"] = "MeshComponent";
        componentJson["mesh"] = serializeMeshComponent(meshComponent.get());
        componentsJson.push_back(componentJson);
    }
    
    // Check for physics component
    auto physicsComponent = actor->getComponent<PhysicsComponent>();
    if (physicsComponent) {
        nlohmann::json componentJson;
        componentJson["type"] = "PhysicsComponent";
        componentJson["physics"] = serializePhysicsComponent(physicsComponent.get());
        componentsJson.push_back(componentJson);
    }
    
    actorJson["components"] = componentsJson;
    
    return actorJson;
}

std::shared_ptr<Actor> ActorSerializer::deserializeActor(const nlohmann::json& json) {
    try {
        // Extract basic properties
        std::string name = "Actor";
        if (json.contains("name")) {
            name = json["name"].get<std::string>();
        }
        
        // Create the actor
        auto actor = std::make_shared<Actor>(name);
        
        // Set active state
        if (json.contains("active")) {
            actor->setActive(json["active"].get<bool>());
        }
        
        // Deserialize transform component
        if (json.contains("transform")) {
            auto transform = actor->getTransform();
            if (transform) {
                deserializeTransformComponent(transform, json["transform"]);
            }
        }
        
        // Deserialize components
        if (json.contains("components") && json["components"].is_array()) {
            for (const auto& componentJson : json["components"]) {
                if (componentJson.contains("type")) {
                    std::string type = componentJson["type"].get<std::string>();
                    
                    if (type == "MeshComponent") {
                        auto meshComponent = actor->getComponent<MeshComponent>();
                        if (!meshComponent) {
                            meshComponent = actor->addComponent<MeshComponent>();
                        }
                        
                        if (componentJson.contains("mesh")) {
                            deserializeMeshComponent(meshComponent.get(), componentJson["mesh"]);
                        }
                    }
                    else if (type == "PhysicsComponent") {
                        auto physicsComponent = actor->getComponent<PhysicsComponent>();
                        if (!physicsComponent) {
                            physicsComponent = actor->addComponent<PhysicsComponent>();
                        }
                        
                        if (componentJson.contains("physics")) {
                            deserializePhysicsComponent(physicsComponent.get(), componentJson["physics"]);
                        }
                    }
                }
            }
        }
        
        return actor;
    }
    catch (const std::exception& e) {
        std::cerr << "Error deserializing actor: " << e.what() << std::endl;
        return nullptr;
    }
}

nlohmann::json ActorSerializer::serializeTransformComponent(const TransformComponent* transform) {
    if (!transform) return nlohmann::json();
    
    nlohmann::json transformJson;
    
    // Serialize position, rotation, scale
    auto position = transform->getPosition();
    transformJson["position"] = nlohmann::json::array({position.x, position.y, position.z});
    
    auto rotation = transform->getRotation();
    transformJson["rotation"] = nlohmann::json::array({rotation.x, rotation.y, rotation.z});
    
    auto scale = transform->getScale();
    transformJson["scale"] = nlohmann::json::array({scale.x, scale.y, scale.z});
    
    return transformJson;
}

void ActorSerializer::deserializeTransformComponent(TransformComponent* transform, const nlohmann::json& json) {
    if (!transform) return;
    
    // Deserialize position
    if (json.contains("position") && json["position"].is_array() && json["position"].size() == 3) {
        glm::vec3 position(
            json["position"][0].get<float>(),
            json["position"][1].get<float>(),
            json["position"][2].get<float>()
        );
        transform->setPosition(position);
    }
    
    // Deserialize rotation
    if (json.contains("rotation") && json["rotation"].is_array() && json["rotation"].size() == 3) {
        glm::vec3 rotation(
            json["rotation"][0].get<float>(),
            json["rotation"][1].get<float>(),
            json["rotation"][2].get<float>()
        );
        transform->setRotation(rotation);
    }
    
    // Deserialize scale
    if (json.contains("scale") && json["scale"].is_array() && json["scale"].size() == 3) {
        glm::vec3 scale(
            json["scale"][0].get<float>(),
            json["scale"][1].get<float>(),
            json["scale"][2].get<float>()
        );
        transform->setScale(scale);
    }
}

nlohmann::json ActorSerializer::serializeMeshComponent(const MeshComponent* component) {
    if (!component) return nlohmann::json();
    
    nlohmann::json meshJson;
    
    meshJson["enabled"] = component->isEnabled();
    
    // TODO: Serialize model reference and material properties when implemented
    
    return meshJson;
}

void ActorSerializer::deserializeMeshComponent(MeshComponent* component, const nlohmann::json& json) {
    if (!component) return;
    
    // Set enabled state
    if (json.contains("enabled")) {
        component->setEnabled(json["enabled"].get<bool>());
    }
    
    // Create and set a default model if one doesn't exist
    if (!component->getModel()) {
        // Determine the appropriate primitive type based on the actor name
        PrimitiveType primitiveType = PrimitiveType::Cube; // Default to cube
        
        if (auto owner = component->getOwner()) {
            std::string name = owner->getName();
            // Convert to lowercase for case-insensitive comparison
            std::transform(name.begin(), name.end(), name.begin(), 
                          [](unsigned char c){ return std::tolower(c); });
            
            if (name.find("sphere") != std::string::npos) {
                primitiveType = PrimitiveType::Sphere;
            } else if (name.find("cube") != std::string::npos) {
                primitiveType = PrimitiveType::Cube;
            } else if (name.find("platform") != std::string::npos) {
                primitiveType = PrimitiveType::Platform;
            } else if (name.find("cylinder") != std::string::npos) {
                primitiveType = PrimitiveType::Cylinder;
            } else if (name.find("cone") != std::string::npos) {
                primitiveType = PrimitiveType::Cone;
            }
        }
        
        auto model = generatePrimitiveMesh(primitiveType);
        component->setModel(model);
    }
    
    // TODO: Deserialize model reference and material properties when implemented
}

nlohmann::json ActorSerializer::serializePhysicsComponent(const PhysicsComponent* component) {
    if (!component) return nlohmann::json();
    
    nlohmann::json physicsJson;
    physicsJson["enabled"] = component->isEnabled();
    
    // Physics system temporarily disabled
    return physicsJson;
}

void ActorSerializer::deserializePhysicsComponent(PhysicsComponent* component, const nlohmann::json& json) {
    if (!component) return;
    
    // Set enabled state
    if (json.contains("enabled")) {
        component->setEnabled(json["enabled"].get<bool>());
    }
    
    // Physics system temporarily disabled
}

} // namespace ohao 