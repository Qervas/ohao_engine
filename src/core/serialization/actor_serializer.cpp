#include "actor_serializer.hpp"
#include "../actor/actor.hpp"
#include "../actor/light_actor.hpp"
#include "../component/light_component.hpp"
#include "../component/transform_component.hpp"
#include "../component/mesh_component.hpp"
#include "../component/physics_component.hpp"
#include "../physics/collision_shape.hpp"
#include "../asset/model.hpp"
#include "../asset/primitive_mesh_generator.hpp"
#include "../../ui/components/console_widget.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace ohao {

// Simple enum for primitive types if it doesn't exist elsewhere
enum class PrimitiveType {
    Cube,
    Sphere,
    Plane,
    Cylinder,
    Cone
};

// Helper function to generate primitive meshes
std::shared_ptr<Model> generatePrimitiveMesh(PrimitiveType type) {
    auto model = std::make_shared<Model>();
    
    switch (type) {
        case PrimitiveType::Cube:
            PrimitiveMeshGenerator::generateCube(*model);
            break;
        case PrimitiveType::Sphere:
            PrimitiveMeshGenerator::generateSphere(*model);
            break;
        case PrimitiveType::Plane:
            PrimitiveMeshGenerator::generatePlane(*model);
            break;
        case PrimitiveType::Cylinder:
            PrimitiveMeshGenerator::generateCylinder(*model);
            break;
        case PrimitiveType::Cone:
            PrimitiveMeshGenerator::generateCone(*model);
            break;
    }
    
    return model;
}

nlohmann::json ActorSerializer::serializeActor(const Actor* actor) {
    if (!actor) return nlohmann::json();
    
    nlohmann::json actorJson;
    
    // Serialize basic properties
    actorJson["id"] = actor->getID();
    actorJson["name"] = actor->getName();
    actorJson["active"] = actor->isActive();
    
    // Serialize parent-child relationship - ensure parent ID is valid
    Actor* parent = actor->getParent();
    actorJson["parentId"] = (parent && parent->getID() > 0) ? parent->getID() : 0;
    
    // Serialize actor type - determine if it's a specialized actor
    std::string actorType = "Actor";
    
    // Try to determine more specific type
    try {
        if (dynamic_cast<const LightActor*>(actor)) {
            actorType = "LightActor";
        }
        // Add other special actor types as needed
    } catch (...) {
        // If type detection fails, just use generic Actor type
    }
    
    actorJson["type"] = actorType;
    
    // Serialize metadata if available
    try {
        // Get all metadata from the actor
        const auto& metadata = actor->getAllMetadata();
        if (!metadata.empty()) {
            actorJson["metadata"] = metadata;
            
            // Add primitive type as a top-level property for easier deserialization
            if (metadata.find("primitive_type") != metadata.end()) {
                actorJson["primitive_type"] = metadata.at("primitive_type");
            }
        }
    } catch (const std::exception& e) {
        // Log error but continue with serialization
        std::cerr << "Error serializing actor metadata: " << e.what() << std::endl;
    }
    
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
    
    // Check for light component
    auto lightComponent = actor->getComponent<LightComponent>();
    if (lightComponent) {
        nlohmann::json componentJson;
        componentJson["type"] = "LightComponent";
        componentJson["light"] = lightComponent->serialize();
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
        
        // Check for actor type to properly create specialized actors
        std::string actorType = "Actor";
        if (json.contains("type")) {
            actorType = json["type"].get<std::string>();
        }
        
        // Create the appropriate actor type
        std::shared_ptr<Actor> actor;
        
        if (actorType == "LightActor") {
            actor = std::make_shared<LightActor>(name);
        } else {
            // Create a descriptive name if we have primitive type information
            std::string primitiveType;
            if (json.contains("primitive_type")) {
                primitiveType = json["primitive_type"].get<std::string>();
                // Create a better name based on the primitive type
                if (name == "Actor" || name == "Object") {
                    name = primitiveType + "_" + std::to_string(json["id"].get<uint64_t>());
                }
            }
            
            actor = std::make_shared<Actor>(name);
            
            // Set primitive_type metadata if available
            if (!primitiveType.empty()) {
                actor->setMetadata("primitive_type", primitiveType);
            }
        }
        
        // Set ID if present
        if (json.contains("id")) {
            uint64_t id = json["id"].get<uint64_t>();
            actor->setID(id); // Assuming Actor class has a setID method
        }
        
        // Set active state
        if (json.contains("active")) {
            actor->setActive(json["active"].get<bool>());
        }
        
        // Import metadata
        if (json.contains("metadata") && json["metadata"].is_object()) {
            for (auto it = json["metadata"].begin(); it != json["metadata"].end(); ++it) {
                actor->setMetadata(it.key(), it.value().get<std::string>());
            }
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
                    else if (type == "LightComponent") {
                        auto lightComponent = actor->getComponent<LightComponent>();
                        if (!lightComponent) {
                            lightComponent = actor->addComponent<LightComponent>();
                        }
                        
                        if (componentJson.contains("light")) {
                            lightComponent->deserialize(componentJson["light"]);
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
    if (!component) {
        OHAO_LOG_ERROR("deserializeMeshComponent: Component is null");
        return;
    }
    
    // Set enabled state
    if (json.contains("enabled")) {
        component->setEnabled(json["enabled"].get<bool>());
    }
    
    // Create and set a default model if one doesn't exist
    if (!component->getModel()) {
        OHAO_LOG("No model set in mesh component, creating primitive");
        
        // Default to cube as fallback
        PrimitiveType primitiveType = PrimitiveType::Cube; 
        std::string typeName = "Cube"; // Default
        
        if (auto owner = component->getOwner()) {
            OHAO_LOG("MeshComponent owner: " + owner->getName() + " (ID: " + std::to_string(owner->getID()) + ")");
            
            // First check metadata for precise primitive_type
            if (owner->hasMetadata("primitive_type")) {
                typeName = owner->getMetadata("primitive_type");
                OHAO_LOG("Found primitive_type metadata: " + typeName);
                
                // Convert to lowercase for case-insensitive comparison
                std::string typeLower = typeName;
                std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), 
                              [](unsigned char c){ return std::tolower(c); });
                
                // Map string primitive type to enum
                if (typeLower == "sphere") {
                    primitiveType = PrimitiveType::Sphere;
                    OHAO_LOG("Creating sphere from metadata for: " + owner->getName());
                }
                else if (typeLower == "cube") {
                    primitiveType = PrimitiveType::Cube;
                    OHAO_LOG("Creating cube from metadata for: " + owner->getName());
                }
                else if (typeLower == "plane") {
                    primitiveType = PrimitiveType::Plane;
                    OHAO_LOG("Creating plane from metadata for: " + owner->getName());
                }
                else if (typeLower == "cylinder") {
                    primitiveType = PrimitiveType::Cylinder;
                    OHAO_LOG("Creating cylinder from metadata for: " + owner->getName());
                }
                else if (typeLower == "cone") {
                    primitiveType = PrimitiveType::Cone;
                    OHAO_LOG("Creating cone from metadata for: " + owner->getName());
                }
            }
            // Fall back to name-based detection if no metadata
            else {
                std::string name = owner->getName();
                OHAO_LOG("No primitive_type metadata, checking name: " + name);
                
                // Convert to lowercase for case-insensitive comparison
                std::transform(name.begin(), name.end(), name.begin(), 
                              [](unsigned char c){ return std::tolower(c); });
                
                if (name.find("sphere") != std::string::npos) {
                    primitiveType = PrimitiveType::Sphere;
                    typeName = "Sphere";
                    OHAO_LOG("Creating sphere based on name: " + owner->getName());
                } else if (name.find("cube") != std::string::npos) {
                    primitiveType = PrimitiveType::Cube;
                    typeName = "Cube";
                    OHAO_LOG("Creating cube based on name: " + owner->getName());
                } else if (name.find("plane") != std::string::npos) {
                    primitiveType = PrimitiveType::Plane;
                    typeName = "Plane";
                    OHAO_LOG("Creating plane based on name: " + owner->getName());
                } else if (name.find("cylinder") != std::string::npos) {
                    primitiveType = PrimitiveType::Cylinder;
                    typeName = "Cylinder";
                    OHAO_LOG("Creating cylinder based on name: " + owner->getName());
                } else if (name.find("cone") != std::string::npos) {
                    primitiveType = PrimitiveType::Cone;
                    typeName = "Cone";
                    OHAO_LOG("Creating cone based on name: " + owner->getName());
                } else {
                    OHAO_LOG("No specific primitive type detected, using default cube");
                }
            }
            
            // Set metadata back on actor if it was detected from name
            if (!owner->hasMetadata("primitive_type")) {
                owner->setMetadata("primitive_type", typeName);
                OHAO_LOG("Setting primitive_type metadata to: " + typeName);
            }
        }
        
        // Generate the appropriate model
        OHAO_LOG("Generating primitive mesh: " + typeName);
        auto model = generatePrimitiveMesh(primitiveType);
        
        // Set the model on the component
        component->setModel(model);
        OHAO_LOG("Model set on component");
    } else {
        OHAO_LOG("Component already has a model");
    }
    
    // TODO: Deserialize model reference and material properties when implemented
}

nlohmann::json ActorSerializer::serializePhysicsComponent(const PhysicsComponent* component) {
    if (!component) return nlohmann::json();
    
    nlohmann::json physicsJson;
    
    physicsJson["enabled"] = component->isEnabled();
    physicsJson["static"] = component->isStatic();
    physicsJson["mass"] = component->getMass();
    physicsJson["friction"] = component->getFriction();
    physicsJson["restitution"] = component->getRestitution();
    
    // Serialize collision shape
    auto shape = component->getCollisionShape();
    if (shape) {
        nlohmann::json shapeJson;
        shapeJson["type"] = static_cast<int>(shape->getType());
        
        // Serialize shape-specific properties
        switch(shape->getType()) {
            case CollisionShape::Type::BOX: {
                // Since we don't have specific shape classes, just use the general methods
                shapeJson["size"] = {
                    shape->getBoxSize().x,
                    shape->getBoxSize().y,
                    shape->getBoxSize().z
                    };
                break;
            }
            case CollisionShape::Type::SPHERE: {
                shapeJson["radius"] = shape->getSphereRadius();
                break;
            }
            case CollisionShape::Type::CAPSULE: {
                shapeJson["radius"] = shape->getCapsuleRadius();
                shapeJson["height"] = shape->getCapsuleHeight();
                break;
            }
            // TODO: Add other shape types as needed
            default:
                break;
        }
        
        physicsJson["shape"] = shapeJson;
    }
    
    return physicsJson;
}

void ActorSerializer::deserializePhysicsComponent(PhysicsComponent* component, const nlohmann::json& json) {
    if (!component) return;
    
    // Set enabled state
    if (json.contains("enabled")) {
        component->setEnabled(json["enabled"].get<bool>());
    }
    
    // Set physics properties
    if (json.contains("static")) {
        component->setStatic(json["static"].get<bool>());
    }
    
    if (json.contains("mass")) {
        component->setMass(json["mass"].get<float>());
    }
    
    if (json.contains("friction")) {
        component->setFriction(json["friction"].get<float>());
    }
    
    if (json.contains("restitution")) {
        component->setRestitution(json["restitution"].get<float>());
    }
    
    // Deserialize collision shape
    if (json.contains("shape") && json["shape"].is_object()) {
        const auto& shapeJson = json["shape"];
        
        if (shapeJson.contains("type")) {
            int shapeType = shapeJson["type"].get<int>();
            
            // Create appropriate shape
            switch(static_cast<CollisionShape::Type>(shapeType)) {
                case CollisionShape::Type::BOX: {
                    glm::vec3 size(1.0f);
                    if (shapeJson.contains("size") && shapeJson["size"].is_array() && 
                        shapeJson["size"].size() == 3) {
                        size = glm::vec3(
                            shapeJson["size"][0].get<float>(),
                            shapeJson["size"][1].get<float>(),
                            shapeJson["size"][2].get<float>()
                        );
                    } else if (shapeJson.contains("halfExtents") && shapeJson["halfExtents"].is_array() && 
                        shapeJson["halfExtents"].size() == 3) {
                        // Support for legacy serialized data
                        size = glm::vec3(
                            shapeJson["halfExtents"][0].get<float>() * 2.0f,
                            shapeJson["halfExtents"][1].get<float>() * 2.0f,
                            shapeJson["halfExtents"][2].get<float>() * 2.0f
                        );
                    }
                    component->createBoxShape(size);
                    break;
                }
                case CollisionShape::Type::SPHERE: {
                    float radius = 1.0f;
                    if (shapeJson.contains("radius")) {
                        radius = shapeJson["radius"].get<float>();
                    }
                    component->createSphereShape(radius);
                    break;
                }
                case CollisionShape::Type::CAPSULE: {
                    float radius = 0.5f;
                    float height = 2.0f;
                    if (shapeJson.contains("radius")) {
                        radius = shapeJson["radius"].get<float>();
                    }
                    if (shapeJson.contains("height")) {
                        height = shapeJson["height"].get<float>();
                    }
                    component->createCapsuleShape(radius, height);
                    break;
                }
                // TODO: Add other shape types as needed
                default:
                    break;
            }
        }
    }
}

} // namespace ohao 