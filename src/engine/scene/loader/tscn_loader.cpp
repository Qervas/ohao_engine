#include "tscn_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/components/light_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/component/transform_component.hpp"
#include "engine/component/component_factory.hpp"

#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <iostream>

// Simple logging macros that don't depend on ConsoleWidget
#define TSCN_LOG(msg) std::cout << "[TscnLoader] " << msg << std::endl
#define TSCN_LOG_ERROR(msg) std::cerr << "[TscnLoader ERROR] " << msg << std::endl

namespace ohao {
namespace loader {

bool TscnLoader::load(const std::string& filepath) {
    m_filepath = filepath;
    m_scene = TscnScene{};
    m_error.clear();

    // Read file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        m_error = "Failed to open file: " + filepath;
        TSCN_LOG_ERROR(m_error);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    TSCN_LOG("Loading .tscn file: " + filepath);

    return parseFile(content);
}

bool TscnLoader::parseFile(const std::string& content) {
    // Split into sections by [brackets]
    std::regex sectionRegex(R"(\[([^\]]+)\])");
    std::sregex_iterator iter(content.begin(), content.end(), sectionRegex);
    std::sregex_iterator end;

    // Find all section headers and their bodies
    std::vector<std::pair<size_t, std::string>> headers;
    for (; iter != end; ++iter) {
        headers.push_back({iter->position(), iter->str(1)});
    }

    // Extract body for each section
    std::vector<std::pair<std::string, std::string>> sections;
    for (size_t i = 0; i < headers.size(); ++i) {
        size_t start = headers[i].first + headers[i].second.length() + 2; // +2 for []
        size_t bodyEnd = (i + 1 < headers.size()) ? headers[i + 1].first : content.length();
        std::string body = content.substr(start, bodyEnd - start);
        sections.push_back({headers[i].second, body});
    }

    // Parse each section
    for (const auto& [header, body] : sections) {
        if (header.find("gd_scene") != std::string::npos) {
            // Scene header, skip
            continue;
        } else if (header.find("sub_resource") != std::string::npos) {
            if (!parseSubResource(header, body)) {
                return false;
            }
        } else if (header.find("node") != std::string::npos) {
            if (!parseNode(header, body)) {
                return false;
            }
        }
    }

    TSCN_LOG("Parsed " + std::to_string(m_scene.meshes.size()) + " meshes, " +
             std::to_string(m_scene.materials.size()) + " materials, " +
             std::to_string(m_scene.nodes.size()) + " nodes");

    return true;
}

bool TscnLoader::parseSubResource(const std::string& header, const std::string& body) {
    // Parse: sub_resource type="BoxMesh" id="BoxMesh_cube"
    std::regex typeRegex(R"(type=\"([^\"]+)\")");
    std::regex idRegex(R"(id=\"([^\"]+)\")");

    std::smatch typeMatch, idMatch;
    std::string type, id;

    if (std::regex_search(header, typeMatch, typeRegex)) {
        type = typeMatch[1];
    }
    if (std::regex_search(header, idMatch, idRegex)) {
        id = idMatch[1];
    }

    if (type.empty() || id.empty()) {
        m_error = "Invalid sub_resource header: " + header;
        return false;
    }

    // Parse mesh types
    if (type == "BoxMesh" || type == "SphereMesh" || type == "PlaneMesh" ||
        type == "CylinderMesh" || type == "CapsuleMesh") {
        TscnMesh mesh;
        mesh.id = id;
        mesh.type = type;

        // Parse properties from body
        std::istringstream stream(body);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("size") != std::string::npos && line.find("Vector3") != std::string::npos) {
                mesh.size = parseVector3(line);
            } else if (line.find("size") != std::string::npos && line.find("Vector2") != std::string::npos) {
                glm::vec2 size2d = parseVector2(line);
                mesh.size = glm::vec3(size2d.x, 0.0f, size2d.y);
            } else if (line.find("radius") != std::string::npos) {
                std::regex numRegex(R"([\d.]+)");
                std::smatch match;
                if (std::regex_search(line, match, numRegex)) {
                    mesh.radius = std::stof(match[0]);
                }
            } else if (line.find("height") != std::string::npos) {
                std::regex numRegex(R"([\d.]+)");
                std::smatch match;
                if (std::regex_search(line, match, numRegex)) {
                    mesh.height = std::stof(match[0]);
                }
            }
        }

        m_scene.meshes[id] = mesh;
    }
    // Parse material types
    else if (type == "StandardMaterial3D") {
        TscnMaterial material;
        material.id = id;

        std::istringstream stream(body);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("albedo_color") != std::string::npos) {
                material.albedo_color = parseColor(line);
            }
        }

        m_scene.materials[id] = material;
    }

    return true;
}

bool TscnLoader::parseNode(const std::string& header, const std::string& body) {
    // Parse: node name="Cube" type="MeshInstance3D" parent="."
    std::regex nameRegex(R"(name=\"([^\"]+)\")");
    std::regex typeRegex(R"(type=\"([^\"]+)\")");
    std::regex parentRegex(R"(parent=\"([^\"]+)\")");

    TscnNode node;

    std::smatch match;
    if (std::regex_search(header, match, nameRegex)) {
        node.name = match[1];
    }
    if (std::regex_search(header, match, typeRegex)) {
        node.type = match[1];
    }
    if (std::regex_search(header, match, parentRegex)) {
        node.parent = match[1];
    }

    // Parse body properties
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));

        if (line.find("transform") != std::string::npos && line.find("Transform3D") != std::string::npos) {
            parseTransform3D(line, node.position, node.rotation, node.scale);
        } else if (line.find("mesh") != std::string::npos && line.find("SubResource") != std::string::npos) {
            node.mesh_ref = parseSubResourceRef(line);
        } else if (line.find("surface_material_override") != std::string::npos) {
            node.material_ref = parseSubResourceRef(line);
        } else if (line.find("shadow_enabled") != std::string::npos) {
            node.shadow_enabled = (line.find("true") != std::string::npos);
        }
        // Physics properties (from OhaoPhysicsBody if present)
        else if (line.find("body_type") != std::string::npos) {
            node.has_physics = true;
            std::regex numRegex(R"((\d+))");
            if (std::regex_search(line, match, numRegex)) {
                node.body_type = std::stoi(match[1]);
            }
        } else if (line.find("shape_type") != std::string::npos) {
            std::regex numRegex(R"((\d+))");
            if (std::regex_search(line, match, numRegex)) {
                node.shape_type = std::stoi(match[1]);
            }
        } else if (line.find("mass") != std::string::npos) {
            std::regex numRegex(R"([\d.]+)");
            if (std::regex_search(line, match, numRegex)) {
                node.mass = std::stof(match[0]);
            }
        } else if (line.find("friction") != std::string::npos) {
            std::regex numRegex(R"([\d.]+)");
            if (std::regex_search(line, match, numRegex)) {
                node.friction = std::stof(match[0]);
            }
        } else if (line.find("restitution") != std::string::npos) {
            std::regex numRegex(R"([\d.]+)");
            if (std::regex_search(line, match, numRegex)) {
                node.restitution = std::stof(match[0]);
            }
        }
    }

    m_scene.nodes.push_back(node);
    return true;
}

glm::vec2 TscnLoader::parseVector2(const std::string& str) {
    // Parse: Vector2(20, 20)
    std::regex vecRegex(R"(Vector2\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\))");
    std::smatch match;
    if (std::regex_search(str, match, vecRegex)) {
        return glm::vec2(std::stof(match[1]), std::stof(match[2]));
    }
    return glm::vec2(1.0f);
}

glm::vec3 TscnLoader::parseVector3(const std::string& str) {
    // Parse: Vector3(1, 1, 1)
    std::regex vecRegex(R"(Vector3\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\))");
    std::smatch match;
    if (std::regex_search(str, match, vecRegex)) {
        return glm::vec3(std::stof(match[1]), std::stof(match[2]), std::stof(match[3]));
    }
    return glm::vec3(1.0f);
}

glm::vec4 TscnLoader::parseColor(const std::string& str) {
    // Parse: Color(0.8, 0.3, 0.2, 1)
    std::regex colorRegex(R"(Color\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\))");
    std::smatch match;
    if (std::regex_search(str, match, colorRegex)) {
        return glm::vec4(std::stof(match[1]), std::stof(match[2]),
                         std::stof(match[3]), std::stof(match[4]));
    }
    return glm::vec4(1.0f);
}

void TscnLoader::parseTransform3D(const std::string& str, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) {
    // Parse: Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0)
    // Format: basis.x.x, basis.x.y, basis.x.z, basis.y.x, basis.y.y, basis.y.z, basis.z.x, basis.z.y, basis.z.z, origin.x, origin.y, origin.z
    std::regex transformRegex(R"(Transform3D\s*\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\))");

    std::smatch match;
    if (std::regex_search(str, match, transformRegex)) {
        // Build 3x3 basis matrix (column-major in Godot)
        glm::mat3 basis;
        basis[0] = glm::vec3(std::stof(match[1]), std::stof(match[2]), std::stof(match[3]));  // column 0 (X axis)
        basis[1] = glm::vec3(std::stof(match[4]), std::stof(match[5]), std::stof(match[6]));  // column 1 (Y axis)
        basis[2] = glm::vec3(std::stof(match[7]), std::stof(match[8]), std::stof(match[9]));  // column 2 (Z axis)

        // Extract position (origin)
        pos = glm::vec3(std::stof(match[10]), std::stof(match[11]), std::stof(match[12]));

        // Extract scale from basis (length of each column)
        scale.x = glm::length(basis[0]);
        scale.y = glm::length(basis[1]);
        scale.z = glm::length(basis[2]);

        // Normalize basis to get rotation matrix
        if (scale.x > 0.0001f) basis[0] /= scale.x;
        if (scale.y > 0.0001f) basis[1] /= scale.y;
        if (scale.z > 0.0001f) basis[2] /= scale.z;

        // Convert rotation matrix to quaternion
        rot = glm::quat_cast(basis);
    }
}

std::string TscnLoader::parseSubResourceRef(const std::string& str) {
    // Parse: SubResource("BoxMesh_cube")
    std::regex refRegex(R"(SubResource\s*\(\s*\"([^\"]+)\"\s*\))");
    std::smatch match;
    if (std::regex_search(str, match, refRegex)) {
        return match[1];
    }
    return "";
}

PrimitiveType TscnLoader::getPrimitiveType(const std::string& godotMeshType) const {
    if (godotMeshType == "BoxMesh") return PrimitiveType::Cube;
    if (godotMeshType == "SphereMesh") return PrimitiveType::Sphere;
    if (godotMeshType == "PlaneMesh") return PrimitiveType::Platform;
    if (godotMeshType == "CylinderMesh") return PrimitiveType::Cylinder;
    if (godotMeshType == "CapsuleMesh") return PrimitiveType::Cylinder; // No capsule, use cylinder
    return PrimitiveType::Empty;
}

bool TscnLoader::createScene(Scene* scene) {
    if (!scene) {
        m_error = "Scene is null";
        TSCN_LOG_ERROR(m_error);
        return false;
    }

    TSCN_LOG("Creating OHAO scene from parsed .tscn data");

    // Process nodes
    for (const auto& node : m_scene.nodes) {
        if (node.type == "Node3D") {
            // Root or grouping node - skip
            continue;
        } else if (node.type == "MeshInstance3D") {
            Actor::Ptr actor = createActorFromNode(scene, node);
            if (actor) {
                setupMeshComponent(actor, node);
                if (node.has_physics) {
                    setupPhysicsComponent(actor, node);
                }
            }
        } else if (node.type == "DirectionalLight3D" || node.type == "OmniLight3D" ||
                   node.type == "SpotLight3D") {
            Actor::Ptr actor = createActorFromNode(scene, node);
            if (actor) {
                setupLightComponent(actor, node);
            }
        } else if (node.type == "Camera3D") {
            // Store camera info for the caller to apply to the renderer
            m_scene.camera.position = node.position;
            m_scene.camera.rotation = node.rotation;
            m_scene.camera.valid = true;
            TSCN_LOG("Camera node found: " + node.name + " at (" +
                     std::to_string(node.position.x) + ", " +
                     std::to_string(node.position.y) + ", " +
                     std::to_string(node.position.z) + ")");
        }
    }

    // Trigger scene buffer update
    scene->updateSceneBuffers();

    return true;
}

Actor::Ptr TscnLoader::createActorFromNode(Scene* scene, const TscnNode& node) {
    Actor::Ptr actor = scene->createActor(node.name);
    if (!actor) {
        TSCN_LOG_ERROR("Failed to create actor: " + node.name);
        return nullptr;
    }

    // Set transform
    auto transform = actor->getTransform();
    if (transform) {
        transform->setPosition(node.position);
        transform->setRotation(node.rotation);
        transform->setScale(node.scale);
    }

    TSCN_LOG("Created actor: " + node.name + " at (" +
             std::to_string(node.position.x) + ", " +
             std::to_string(node.position.y) + ", " +
             std::to_string(node.position.z) + ")");

    return actor;
}

void TscnLoader::setupMeshComponent(Actor::Ptr actor, const TscnNode& node) {
    if (node.mesh_ref.empty()) return;

    auto it = m_scene.meshes.find(node.mesh_ref);
    if (it == m_scene.meshes.end()) {
        TSCN_LOG_ERROR("Mesh not found: " + node.mesh_ref);
        return;
    }

    const TscnMesh& mesh = it->second;

    // Add mesh component
    auto meshComp = actor->addComponent<MeshComponent>();
    if (!meshComp) return;

    // Generate mesh model based on Godot type
    PrimitiveType primType = getPrimitiveType(mesh.type);
    auto model = ComponentFactory::generateMeshForPrimitive(primType);
    if (model) {
        meshComp->setModel(model);
    }

    // Add material component for color
    if (!node.material_ref.empty()) {
        auto matIt = m_scene.materials.find(node.material_ref);
        if (matIt != m_scene.materials.end()) {
            auto materialComp = actor->addComponent<MaterialComponent>();
            if (materialComp) {
                glm::vec3 color(matIt->second.albedo_color);
                materialComp->getMaterial().baseColor = color;
            }
        }
    }

    TSCN_LOG("Setup mesh component: " + mesh.type + " for " + actor->getName());
}

void TscnLoader::setupPhysicsComponent(Actor::Ptr actor, const TscnNode& node) {
    auto physicsComp = actor->addComponent<PhysicsComponent>();
    if (!physicsComp) return;

    // Set body type
    physics::dynamics::RigidBodyType bodyType = physics::dynamics::RigidBodyType::DYNAMIC;
    switch (node.body_type) {
        case 0: bodyType = physics::dynamics::RigidBodyType::DYNAMIC; break;
        case 1: bodyType = physics::dynamics::RigidBodyType::STATIC; break;
        case 2: bodyType = physics::dynamics::RigidBodyType::KINEMATIC; break;
    }
    physicsComp->setRigidBodyType(bodyType);

    // Set collision shape based on mesh type
    if (!node.mesh_ref.empty()) {
        auto it = m_scene.meshes.find(node.mesh_ref);
        if (it != m_scene.meshes.end()) {
            const TscnMesh& mesh = it->second;
            if (mesh.type == "BoxMesh") {
                physicsComp->createBoxShape(mesh.size * 0.5f); // Half extents
            } else if (mesh.type == "SphereMesh") {
                physicsComp->createSphereShape(mesh.radius);
            } else if (mesh.type == "PlaneMesh") {
                // Use box shape for plane (thin box)
                physicsComp->createBoxShape(mesh.size.x * 0.5f, 0.1f, mesh.size.z * 0.5f);
            } else if (mesh.type == "CylinderMesh") {
                physicsComp->createCylinderShape(mesh.radius, mesh.height);
            } else if (mesh.type == "CapsuleMesh") {
                physicsComp->createCapsuleShape(mesh.radius, mesh.height);
            }
        }
    }

    physicsComp->setMass(node.mass);
    physicsComp->setFriction(node.friction);
    physicsComp->setRestitution(node.restitution);

    TSCN_LOG("Setup physics component for " + actor->getName() +
             " (type=" + std::to_string(node.body_type) + ", mass=" + std::to_string(node.mass) + ")");
}

void TscnLoader::setupLightComponent(Actor::Ptr actor, const TscnNode& node) {
    auto lightComp = actor->addComponent<LightComponent>();
    if (!lightComp) return;

    // Set light type based on Godot node type
    if (node.type == "DirectionalLight3D") {
        lightComp->setLightType(LightType::Directional);
        // Set direction from transform rotation
        glm::vec3 forward = node.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        lightComp->setDirection(forward);
    } else if (node.type == "OmniLight3D") {
        lightComp->setLightType(LightType::Point);
    } else if (node.type == "SpotLight3D") {
        lightComp->setLightType(LightType::Spot);
    }

    // Note: shadow_enabled is stored but not yet supported in LightComponent
    TSCN_LOG("Setup light: " + node.name + " (type=" + node.type + ", shadow=" +
             (node.shadow_enabled ? "true" : "false") + ")");
}

}} // namespace ohao::loader
