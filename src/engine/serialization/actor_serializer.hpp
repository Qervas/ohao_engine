#pragma once

#include <memory>
#include <string>
#include <nlohmann/json_fwd.hpp>

namespace ohao {

class Actor;
class TransformComponent;
class MeshComponent;
class PhysicsComponent;

/**
 * @class ActorSerializer
 * @brief Handles serialization and deserialization of Actor objects to and from JSON
 * 
 * This class provides functionality to serialize an Actor to JSON and deserialize
 * an Actor from JSON. It also handles serialization of components.
 */
class ActorSerializer {
public:
    /**
     * @brief Serialize an actor to JSON
     * @param actor The actor to serialize
     * @return JSON representation of the actor
     */
    static nlohmann::json serializeActor(const Actor* actor);
    
    /**
     * @brief Deserialize an actor from JSON
     * @param json JSON representation of the actor
     * @return Deserialized actor
     */
    static std::shared_ptr<Actor> deserializeActor(const nlohmann::json& json);
    
private:
    // Component serialization helpers
    static nlohmann::json serializeTransformComponent(const TransformComponent* component);
    static void deserializeTransformComponent(TransformComponent* component, const nlohmann::json& json);
    
    static nlohmann::json serializeMeshComponent(const MeshComponent* component);
    static void deserializeMeshComponent(MeshComponent* component, const nlohmann::json& json);
    
    static nlohmann::json serializePhysicsComponent(const PhysicsComponent* component);
    static void deserializePhysicsComponent(PhysicsComponent* component, const nlohmann::json& json);
};

} // namespace ohao 