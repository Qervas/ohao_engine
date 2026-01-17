#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "engine/actor/actor.hpp"
#include "engine/component/component_factory.hpp"

namespace ohao {

class Scene;

namespace loader {

// Parsed mesh data from .tscn
struct TscnMesh {
    std::string id;
    std::string type;  // "BoxMesh", "SphereMesh", "PlaneMesh", etc.
    glm::vec3 size{1.0f};
    float radius = 0.5f;
    float height = 1.0f;
};

// Parsed material data from .tscn
struct TscnMaterial {
    std::string id;
    glm::vec4 albedo_color{1.0f};
};

// Parsed node from .tscn
struct TscnNode {
    std::string name;
    std::string type;  // "Node3D", "MeshInstance3D", "Camera3D", etc.
    std::string parent;

    // Transform (from Transform3D)
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    // References
    std::string mesh_ref;      // SubResource("...")
    std::string material_ref;  // SubResource("...")

    // Light properties
    bool shadow_enabled = false;

    // Physics properties (from OhaoPhysicsBody)
    bool has_physics = false;
    int body_type = 0;  // 0=Dynamic, 1=Static, 2=Kinematic
    int shape_type = 0; // 0=Box, 1=Sphere, 2=Capsule, 3=Mesh
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
};

// Camera info extracted from .tscn
struct TscnCamera {
    glm::vec3 position{0.0f, 5.0f, 8.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool valid = false;
};

// Result of parsing a .tscn file
struct TscnScene {
    std::unordered_map<std::string, TscnMesh> meshes;
    std::unordered_map<std::string, TscnMaterial> materials;
    std::vector<TscnNode> nodes;
    TscnCamera camera;
};

/**
 * TscnLoader - Loads Godot .tscn scene files into OHAO engine
 *
 * Usage:
 *   TscnLoader loader;
 *   if (loader.load("path/to/scene.tscn")) {
 *       loader.createScene(ohaoScene);
 *   }
 */
class TscnLoader {
public:
    TscnLoader() = default;
    ~TscnLoader() = default;

    // Load and parse a .tscn file
    bool load(const std::string& filepath);

    // Create OHAO actors from parsed scene
    bool createScene(Scene* scene);

    // Get parsed data (for inspection)
    const TscnScene& getParsedScene() const { return m_scene; }

    // Get last error message
    const std::string& getError() const { return m_error; }

private:
    TscnScene m_scene;
    std::string m_error;
    std::string m_filepath;

    // Parsing helpers
    bool parseFile(const std::string& content);
    bool parseSubResource(const std::string& header, const std::string& body);
    bool parseNode(const std::string& header, const std::string& body);

    // Value parsing
    glm::vec2 parseVector2(const std::string& str);
    glm::vec3 parseVector3(const std::string& str);
    glm::vec4 parseColor(const std::string& str);
    void parseTransform3D(const std::string& str, glm::vec3& pos, glm::quat& rot, glm::vec3& scale);
    std::string parseSubResourceRef(const std::string& str);

    // Scene building
    Actor::Ptr createActorFromNode(Scene* scene, const TscnNode& node);
    void setupMeshComponent(Actor::Ptr actor, const TscnNode& node);
    void setupPhysicsComponent(Actor::Ptr actor, const TscnNode& node);
    void setupLightComponent(Actor::Ptr actor, const TscnNode& node);

    // Convert Godot mesh type to OHAO PrimitiveType
    PrimitiveType getPrimitiveType(const std::string& godotMeshType) const;
};

}} // namespace ohao::loader
