#include "scene_sync.h"
#include "ohao_physics_body.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/cylinder_mesh.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/primitive_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

#include "renderer/offscreen/offscreen_renderer.hpp"
#include "renderer/camera/camera.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/loader/tscn_loader.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/component_factory.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/light_component.hpp"
#include "renderer/components/material_component.hpp"
#include "engine/asset/model.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace godot {

static glm::vec3 to_glm(const Vector3& v) {
    return glm::vec3(v.x, v.y, v.z);
}

static glm::vec3 to_glm_color(const Color& c) {
    return glm::vec3(c.r, c.g, c.b);
}

SceneSync::SceneSync() {}

int SceneSync::syncFromGodot(Node* root_node, ohao::Scene* scene, ohao::OffscreenRenderer* renderer) {
    if (!root_node || !scene || !renderer) {
        UtilityFunctions::printerr("[OHAO] syncFromGodot: null argument!");
        return -1;
    }

    UtilityFunctions::print("[OHAO] Syncing from Godot scene: ", root_node->get_name());

    // First pass: count objects without modifying scene
    m_synced_object_count = 0;
    countSyncableObjects(root_node);

    if (m_synced_object_count == 0) {
        UtilityFunctions::print("[OHAO] No syncable objects found in Godot scene (need MeshInstance3D, lights, etc.)");
        UtilityFunctions::print("[OHAO] Keeping existing OHAO scene. Add 3D objects in Godot's 3D editor first.");
        return -1;
    }

    // Clear existing OHAO scene
    scene->removeAllActors();
    m_synced_object_count = 0;

    // Traverse the Godot scene tree and create OHAO actors
    traverseAndSync(root_node, scene, renderer);

    // Debug: list actors
    UtilityFunctions::print("[OHAO] === Actors in scene after sync ===");
    for (const auto& [actorId, actor] : scene->getAllActors()) {
        auto lightComp = actor->getComponent<ohao::LightComponent>();
        if (lightComp) {
            auto pos = actor->getTransform()->getPosition();
            auto dir = lightComp->getDirection();
            UtilityFunctions::print("[OHAO]   LIGHT: '", actor->getName().c_str(), "' type=",
                                    static_cast<int>(lightComp->getLightType()),
                                    " pos=(", pos.x, ",", pos.y, ",", pos.z, ")",
                                    " dir=(", dir.x, ",", dir.y, ",", dir.z, ")");
        } else {
            UtilityFunctions::print("[OHAO]   Actor: '", actor->getName().c_str(), "'");
        }
    }
    UtilityFunctions::print("[OHAO] === End actor list ===");

    // Rebuild buffers
    if (renderer->updateSceneBuffers()) {
        UtilityFunctions::print("[OHAO] Sync complete: ", m_synced_object_count, " objects synced");
    } else {
        UtilityFunctions::print("[OHAO] Sync complete but no renderable meshes found");
    }

    return m_synced_object_count;
}

void SceneSync::countSyncableObjects(Node* node) {
    if (!node) return;

    if (Object::cast_to<MeshInstance3D>(node)) {
        MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(node);
        if (mesh_instance->get_mesh().is_valid()) {
            m_synced_object_count++;
        }
    }
    else if (Object::cast_to<DirectionalLight3D>(node) ||
             Object::cast_to<OmniLight3D>(node) ||
             Object::cast_to<SpotLight3D>(node)) {
        m_synced_object_count++;
    }

    int child_count = node->get_child_count();
    for (int i = 0; i < child_count; i++) {
        countSyncableObjects(node->get_child(i));
    }
}

void SceneSync::traverseAndSync(Node* node, ohao::Scene* scene, ohao::OffscreenRenderer* renderer) {
    if (!node) return;

    Node3D* node3d = Object::cast_to<Node3D>(node);
    if (node3d) {
        Transform3D transform = node3d->get_global_transform();
        Vector3 position = transform.origin;
        Vector3 rotation = transform.basis.get_euler();
        Vector3 scale = transform.basis.get_scale();
        String name = node->get_name();

        // MeshInstance3D
        MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(node);
        if (mesh_instance) {
            Ref<Mesh> mesh = mesh_instance->get_mesh();
            if (mesh.is_valid()) {
                Color color(0.8f, 0.8f, 0.8f, 1.0f);

                Ref<Material> mat = mesh_instance->get_surface_override_material(0);
                if (!mat.is_valid() && mesh.is_valid()) {
                    mat = mesh->surface_get_material(0);
                }
                if (mat.is_valid()) {
                    StandardMaterial3D* std_mat = Object::cast_to<StandardMaterial3D>(mat.ptr());
                    if (std_mat) {
                        color = std_mat->get_albedo();
                    }
                }

                BoxMesh* box_mesh = Object::cast_to<BoxMesh>(mesh.ptr());
                SphereMesh* sphere_mesh = Object::cast_to<SphereMesh>(mesh.ptr());
                CylinderMesh* cylinder_mesh = Object::cast_to<CylinderMesh>(mesh.ptr());
                PlaneMesh* plane_mesh = Object::cast_to<PlaneMesh>(mesh.ptr());

                if (box_mesh) {
                    Vector3 mesh_size = box_mesh->get_size();
                    Vector3 final_scale = Vector3(scale.x * mesh_size.x, scale.y * mesh_size.y, scale.z * mesh_size.z);
                    addCube(scene, name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (sphere_mesh) {
                    float radius = sphere_mesh->get_radius();
                    Vector3 final_scale = Vector3(scale.x * radius * 2.0f, scale.y * radius * 2.0f, scale.z * radius * 2.0f);
                    addSphere(scene, name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (cylinder_mesh) {
                    float radius = cylinder_mesh->get_top_radius();
                    float height = cylinder_mesh->get_height();
                    Vector3 final_scale = Vector3(scale.x * radius * 2.0f, scale.y * height, scale.z * radius * 2.0f);
                    addCylinder(scene, name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else if (plane_mesh) {
                    Vector2 mesh_size = plane_mesh->get_size();
                    Vector3 final_scale = Vector3(scale.x * mesh_size.x, scale.y, scale.z * mesh_size.y);
                    addPlane(scene, name, position, rotation * (180.0f / Math_PI), final_scale, color);
                    m_synced_object_count++;
                }
                else {
                    UtilityFunctions::print("[OHAO] Unknown mesh type for '", name, "', treating as cube");
                    addCube(scene, name, position, rotation * (180.0f / Math_PI), scale, color);
                    m_synced_object_count++;
                }
            }
        }

        // DirectionalLight3D
        DirectionalLight3D* dir_light = Object::cast_to<DirectionalLight3D>(node);
        if (dir_light) {
            Color color = dir_light->get_color();
            float intensity = dir_light->get_param(Light3D::PARAM_ENERGY);
            Vector3 direction = -transform.basis.get_column(2);
            addDirectionalLight(scene, name, position, direction, color, intensity);
            m_synced_object_count++;
        }

        // OmniLight3D (point light)
        OmniLight3D* omni_light = Object::cast_to<OmniLight3D>(node);
        if (omni_light) {
            Color color = omni_light->get_color();
            float intensity = omni_light->get_param(Light3D::PARAM_ENERGY);
            float range = omni_light->get_param(Light3D::PARAM_RANGE);
            addPointLight(scene, name, position, color, intensity, range);
            m_synced_object_count++;
        }

        // SpotLight3D
        SpotLight3D* spot_light = Object::cast_to<SpotLight3D>(node);
        if (spot_light) {
            UtilityFunctions::print("[OHAO] SpotLight3D '", name, "' not fully supported yet, treating as point light");
            Color color = spot_light->get_color();
            float intensity = spot_light->get_param(Light3D::PARAM_ENERGY);
            float range = spot_light->get_param(Light3D::PARAM_RANGE);
            addPointLight(scene, name, position, color, intensity, range);
            m_synced_object_count++;
        }

        // OhaoPhysicsBody
        OhaoPhysicsBody* physics_body = Object::cast_to<OhaoPhysicsBody>(node);
        if (physics_body) {
            if (!physics_body->is_in_physics_world()) {
                physics_body->add_to_physics_world();
            }
            m_synced_object_count++;
        }

        // Camera3D
        Camera3D* camera = Object::cast_to<Camera3D>(node);
        if (camera && renderer) {
            ohao::Camera& ohao_cam = renderer->getCamera();
            ohao_cam.setPosition(to_glm(position));
            ohao_cam.setRotation(glm::degrees(rotation.x), glm::degrees(rotation.y));
            UtilityFunctions::print("[OHAO] Camera synced at position: (", position.x, ", ", position.y, ", ", position.z, ")");
        }
    }

    // Recurse
    int child_count = node->get_child_count();
    for (int i = 0; i < child_count; i++) {
        traverseAndSync(node->get_child(i), scene, renderer);
    }
}

void SceneSync::loadTscn(const String& path, ohao::Scene* scene, ohao::OffscreenRenderer* renderer) {
    if (!scene) {
        UtilityFunctions::printerr("[OHAO] Scene not initialized!");
        return;
    }

    std::string filepath = path.utf8().get_data();
    UtilityFunctions::print("[OHAO] Loading scene: ", path);

    ohao::loader::TscnLoader loader;
    if (loader.load(filepath)) {
        scene->removeAllActors();

        if (loader.createScene(scene)) {
            UtilityFunctions::print("[OHAO] Scene loaded successfully");

            const auto& parsed = loader.getParsedScene();
            if (parsed.camera.valid && renderer) {
                auto& camera = renderer->getCamera();
                camera.setPosition(parsed.camera.position);
                glm::vec3 euler = glm::eulerAngles(parsed.camera.rotation);
                camera.setRotation(glm::degrees(euler.x), glm::degrees(euler.y));
            }
        } else {
            UtilityFunctions::printerr("[OHAO] Failed to create scene: ", loader.getError().c_str());
        }
    } else {
        UtilityFunctions::printerr("[OHAO] Failed to load .tscn: ", loader.getError().c_str());
    }
}

void SceneSync::clearScene(ohao::Scene* scene) {
    if (!scene) {
        UtilityFunctions::printerr("[OHAO] Scene not initialized!");
        return;
    }
    scene->removeAllActors();
    UtilityFunctions::print("[OHAO] Scene cleared");
}

void SceneSync::addPrimitive(ohao::Scene* scene, int primitiveType, const String& name,
                              const Vector3& position, const Vector3& rotation,
                              const Vector3& scale, const Color& color) {
    if (!scene) return;
    std::string actorName = name.utf8().get_data();
    auto actor = scene->createActorWithComponents(actorName, static_cast<ohao::PrimitiveType>(primitiveType));
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
            transform->setRotation(glm::quat(glm::radians(to_glm(rotation))));
            transform->setScale(to_glm(scale));
        }
        auto material = actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = to_glm_color(color);
        }
    }
}

void SceneSync::addCube(ohao::Scene* scene, const String& name, const Vector3& position,
                        const Vector3& rotation, const Vector3& scale, const Color& color) {
    addPrimitive(scene, static_cast<int>(ohao::PrimitiveType::Cube), name, position, rotation, scale, color);
}

void SceneSync::addSphere(ohao::Scene* scene, const String& name, const Vector3& position,
                          const Vector3& rotation, const Vector3& scale, const Color& color) {
    addPrimitive(scene, static_cast<int>(ohao::PrimitiveType::Sphere), name, position, rotation, scale, color);
}

void SceneSync::addPlane(ohao::Scene* scene, const String& name, const Vector3& position,
                         const Vector3& rotation, const Vector3& scale, const Color& color) {
    addPrimitive(scene, static_cast<int>(ohao::PrimitiveType::Platform), name, position, rotation, scale, color);
}

void SceneSync::addCylinder(ohao::Scene* scene, const String& name, const Vector3& position,
                            const Vector3& rotation, const Vector3& scale, const Color& color) {
    addPrimitive(scene, static_cast<int>(ohao::PrimitiveType::Cylinder), name, position, rotation, scale, color);
}

void SceneSync::addDirectionalLight(ohao::Scene* scene, const String& name, const Vector3& position,
                                     const Vector3& direction, const Color& color, float intensity) {
    if (!scene) {
        UtilityFunctions::printerr("[OHAO] addDirectionalLight: scene is null!");
        return;
    }
    std::string actorName = name.utf8().get_data();
    UtilityFunctions::print("[OHAO] Adding directional light '", name, "' pos=(", position.x, ",", position.y, ",", position.z,
                            ") dir=(", direction.x, ",", direction.y, ",", direction.z, ") intensity=", intensity);

    auto actor = scene->createActorWithComponents(actorName, ohao::PrimitiveType::DirectionalLight);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
        }
        auto light = actor->getComponent<ohao::LightComponent>();
        if (light) {
            light->setDirection(glm::normalize(to_glm(direction)));
            light->setColor(to_glm_color(color));
            light->setIntensity(intensity);
            UtilityFunctions::print("[OHAO] Light component configured successfully");
        } else {
            UtilityFunctions::printerr("[OHAO] Failed to get LightComponent from actor!");
        }
        UtilityFunctions::print("[OHAO] Scene now has ", static_cast<int>(scene->getAllActors().size()), " actors");
    } else {
        UtilityFunctions::printerr("[OHAO] Failed to create directional light actor!");
    }
}

void SceneSync::addPointLight(ohao::Scene* scene, const String& name, const Vector3& position,
                               const Color& color, float intensity, float range) {
    if (!scene) return;
    std::string actorName = name.utf8().get_data();
    auto actor = scene->createActorWithComponents(actorName, ohao::PrimitiveType::PointLight);
    if (actor) {
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(to_glm(position));
        }
        auto light = actor->getComponent<ohao::LightComponent>();
        if (light) {
            light->setColor(to_glm_color(color));
            light->setIntensity(intensity);
            light->setRange(range);
        }
    }
}

bool SceneSync::importModel(ohao::Scene* scene, ohao::OffscreenRenderer* renderer, const std::string& filepath) {
    if (!scene || !renderer) {
        UtilityFunctions::printerr("[OHAO] Cannot import: scene or renderer not initialized!");
        return false;
    }

    UtilityFunctions::print("[OHAO] Importing model: ", filepath.c_str());

    auto model = std::make_shared<ohao::Model>();
    bool loaded = false;

    if (filepath.size() >= 4) {
        std::string ext = filepath.substr(filepath.find_last_of('.'));
        if (ext == ".obj" || ext == ".OBJ") {
            loaded = model->loadFromOBJ(filepath);
        } else if (ext == ".gltf" || ext == ".glb" || ext == ".GLTF" || ext == ".GLB") {
            loaded = model->loadFromGLTF(filepath);
        } else {
            UtilityFunctions::printerr("[OHAO] Unsupported format: ", filepath.c_str());
            return false;
        }
    }

    if (!loaded) {
        UtilityFunctions::printerr("[OHAO] Failed to load model: ", filepath.c_str());
        return false;
    }

    std::string name = filepath.substr(filepath.find_last_of("/\\") + 1);
    name = name.substr(0, name.find_last_of('.'));

    auto actor = scene->createActor(name);
    if (actor) {
        actor->setModel(model);
        auto transform = actor->getTransform();
        if (transform) {
            transform->setPosition(glm::vec3(0.0f));
        }
    }

    finishSync(renderer);
    UtilityFunctions::print("[OHAO] Model imported: ", name.c_str());
    return true;
}

void SceneSync::finishSync(ohao::OffscreenRenderer* renderer) {
    if (!renderer) {
        UtilityFunctions::printerr("[OHAO] Renderer not initialized!");
        return;
    }
    if (renderer->updateSceneBuffers()) {
        UtilityFunctions::print("[OHAO] finishSync: buffers rebuilt OK (hasSceneMeshes=true)");
    } else {
        UtilityFunctions::printerr("[OHAO] finishSync: updateSceneBuffers FAILED (no meshes?)");
    }
}

std::string SceneSync::resolveResPath(const String& res_path) {
    // Convert res:// to absolute filesystem path using Godot's ProjectSettings
    String globalized = ProjectSettings::get_singleton()->globalize_path(res_path);
    return std::string(globalized.utf8().get_data());
}

} // namespace godot
