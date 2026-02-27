#include "ohao_mesh_instance.h"
#include "ohao_viewport.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "engine/component/component_factory.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/offscreen/offscreen_renderer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace godot {

void OhaoMeshInstance::_bind_methods() {
    // Mesh type
    ClassDB::bind_method(D_METHOD("set_mesh_type", "type"), &OhaoMeshInstance::set_mesh_type);
    ClassDB::bind_method(D_METHOD("get_mesh_type"), &OhaoMeshInstance::get_mesh_type);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_type", PROPERTY_HINT_ENUM, "Cube,Sphere,Cylinder,Plane"),
                 "set_mesh_type", "get_mesh_type");

    // Color
    ClassDB::bind_method(D_METHOD("set_mesh_color", "color"), &OhaoMeshInstance::set_mesh_color);
    ClassDB::bind_method(D_METHOD("get_mesh_color"), &OhaoMeshInstance::get_mesh_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mesh_color"), "set_mesh_color", "get_mesh_color");

    // Scale
    ClassDB::bind_method(D_METHOD("set_mesh_scale", "scale"), &OhaoMeshInstance::set_mesh_scale);
    ClassDB::bind_method(D_METHOD("get_mesh_scale"), &OhaoMeshInstance::get_mesh_scale);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "mesh_scale"), "set_mesh_scale", "get_mesh_scale");

    // PBR
    ClassDB::bind_method(D_METHOD("set_metallic", "metallic"), &OhaoMeshInstance::set_metallic);
    ClassDB::bind_method(D_METHOD("get_metallic"), &OhaoMeshInstance::get_metallic);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_metallic", "get_metallic");

    ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &OhaoMeshInstance::set_roughness);
    ClassDB::bind_method(D_METHOD("get_roughness"), &OhaoMeshInstance::get_roughness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"),
                 "set_roughness", "get_roughness");

    // Actor name override
    ClassDB::bind_method(D_METHOD("set_actor_name_override", "name"), &OhaoMeshInstance::set_actor_name_override);
    ClassDB::bind_method(D_METHOD("get_actor_name_override"), &OhaoMeshInstance::get_actor_name_override);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "actor_name_override"), "set_actor_name_override", "get_actor_name_override");

    // Runtime queries
    ClassDB::bind_method(D_METHOD("is_synced"), &OhaoMeshInstance::is_synced);
    ClassDB::bind_method(D_METHOD("get_resolved_actor_name"), &OhaoMeshInstance::get_resolved_actor_name);

    // Enum constants
    BIND_ENUM_CONSTANT(MESH_CUBE);
    BIND_ENUM_CONSTANT(MESH_SPHERE);
    BIND_ENUM_CONSTANT(MESH_CYLINDER);
    BIND_ENUM_CONSTANT(MESH_PLANE);
}

OhaoMeshInstance::OhaoMeshInstance() {
}

OhaoMeshInstance::~OhaoMeshInstance() {
    destroy_ohao_actor();
}

OhaoViewport* OhaoMeshInstance::find_viewport() const {
    Node* current = get_parent();
    while (current) {
        OhaoViewport* viewport = Object::cast_to<OhaoViewport>(current);
        if (viewport) return viewport;
        current = current->get_parent();
    }
    return nullptr;
}

String OhaoMeshInstance::resolve_actor_name() const {
    if (!m_actor_name_override.is_empty()) {
        return m_actor_name_override;
    }
    // Use parent node's name (required for weapon hitscan matching)
    Node* parent = get_parent();
    if (parent) {
        return parent->get_name();
    }
    return get_name();
}

String OhaoMeshInstance::get_resolved_actor_name() const {
    return resolve_actor_name();
}

void OhaoMeshInstance::_ready() {
    UtilityFunctions::print("[OHAO] MeshInstance ready: ", resolve_actor_name());
    create_ohao_actor();
}

void OhaoMeshInstance::_notification(int p_what) {
    if (p_what == NOTIFICATION_EXIT_TREE) {
        destroy_ohao_actor();
    }
}

void OhaoMeshInstance::_process(double delta) {
    if (!m_synced || !m_actor) return;

    auto transform = m_actor->getTransform();
    if (!transform) return;

    // Sync Godot transform -> OHAO actor
    // Walk up to accumulate the 3D transform chain manually,
    // since get_global_position() fails when any ancestor is a Control.
    Transform3D gt = get_transform();
    Node* p = get_parent();
    while (p) {
        Node3D* p3d = Object::cast_to<Node3D>(p);
        if (p3d) {
            gt = p3d->get_transform() * gt;
        }
        p = p->get_parent();
    }

    Vector3 pos = gt.origin;
    Quaternion rot = gt.basis.get_quaternion();
    Vector3 node_scale = gt.basis.get_scale();

    transform->setPosition(glm::vec3(pos.x, pos.y, pos.z));
    transform->setRotation(glm::quat(rot.w, rot.x, rot.y, rot.z));
    // Combine node transform scale with mesh_scale
    transform->setScale(glm::vec3(
        node_scale.x * m_mesh_scale.x,
        node_scale.y * m_mesh_scale.y,
        node_scale.z * m_mesh_scale.z
    ));

    // Apply dirty material changes
    if (m_material_dirty) {
        auto material = m_actor->getComponent<ohao::MaterialComponent>();
        if (material) {
            material->getMaterial().baseColor = glm::vec3(m_mesh_color.r, m_mesh_color.g, m_mesh_color.b);
            material->getMaterial().metallic = m_metallic;
            material->getMaterial().roughness = m_roughness;
        }
        m_material_dirty = false;
    }
}

void OhaoMeshInstance::create_ohao_actor() {
    if (m_synced) return;

    m_viewport = find_viewport();
    if (!m_viewport || !m_viewport->is_renderer_initialized()) {
        UtilityFunctions::print("[OHAO] MeshInstance: No initialized OhaoViewport found in parent tree");
        return;
    }

    m_scene = m_viewport->get_ohao_scene();
    m_renderer = m_viewport->get_ohao_renderer();
    if (!m_scene || !m_renderer) {
        UtilityFunctions::print("[OHAO] MeshInstance: Scene or renderer not available");
        return;
    }

    // Map MeshType to PrimitiveType
    ohao::PrimitiveType primitiveType;
    switch (m_mesh_type) {
        case MESH_SPHERE:   primitiveType = ohao::PrimitiveType::Sphere; break;
        case MESH_CYLINDER: primitiveType = ohao::PrimitiveType::Cylinder; break;
        case MESH_PLANE:    primitiveType = ohao::PrimitiveType::Platform; break;
        default:            primitiveType = ohao::PrimitiveType::Cube; break;
    }

    String actorNameGd = resolve_actor_name();
    std::string actorName = actorNameGd.utf8().get_data();

    m_actor = m_scene->createActorWithComponents(actorName, primitiveType);
    if (!m_actor) {
        UtilityFunctions::printerr("[OHAO] MeshInstance: Failed to create actor '", actorNameGd, "'");
        return;
    }

    // Set initial transform — accumulate 3D chain manually
    Transform3D gt = get_transform();
    Node* par = get_parent();
    while (par) {
        Node3D* par3d = Object::cast_to<Node3D>(par);
        if (par3d) {
            gt = par3d->get_transform() * gt;
        }
        par = par->get_parent();
    }

    auto transform = m_actor->getTransform();
    if (transform) {
        transform->setPosition(glm::vec3(gt.origin.x, gt.origin.y, gt.origin.z));
        Quaternion rot = gt.basis.get_quaternion();
        transform->setRotation(glm::quat(rot.w, rot.x, rot.y, rot.z));
        Vector3 node_scale = gt.basis.get_scale();
        transform->setScale(glm::vec3(
            node_scale.x * m_mesh_scale.x,
            node_scale.y * m_mesh_scale.y,
            node_scale.z * m_mesh_scale.z
        ));
    }

    // Set material
    auto material = m_actor->getComponent<ohao::MaterialComponent>();
    if (material) {
        material->getMaterial().baseColor = glm::vec3(m_mesh_color.r, m_mesh_color.g, m_mesh_color.b);
        material->getMaterial().metallic = m_metallic;
        material->getMaterial().roughness = m_roughness;
    }

    // Rebuild GPU buffers so the new mesh appears
    m_renderer->updateSceneBuffers();

    m_synced = true;
    UtilityFunctions::print("[OHAO] MeshInstance '", actorNameGd, "' created (",
        m_mesh_type == MESH_CUBE ? "cube" :
        m_mesh_type == MESH_SPHERE ? "sphere" :
        m_mesh_type == MESH_CYLINDER ? "cylinder" : "plane", ")");
}

void OhaoMeshInstance::destroy_ohao_actor() {
    if (!m_synced) return;

    if (m_scene && m_actor) {
        std::string actorName = m_actor->getName();
        m_scene->removeActor(actorName);
        UtilityFunctions::print("[OHAO] MeshInstance '", String(actorName.c_str()), "' removed");
    }

    // Rebuild GPU buffers so the mesh disappears
    if (m_renderer) {
        m_renderer->updateSceneBuffers();
    }

    m_actor.reset();
    m_scene = nullptr;
    m_renderer = nullptr;
    m_viewport = nullptr;
    m_synced = false;
}

// --- Property setters ---

void OhaoMeshInstance::set_mesh_type(int type) {
    m_mesh_type = static_cast<MeshType>(type);
    // If already synced, would need to recreate actor (mesh type is immutable after creation)
}

void OhaoMeshInstance::set_mesh_color(const Color& color) {
    m_mesh_color = color;
    m_material_dirty = true;
}

void OhaoMeshInstance::set_mesh_scale(const Vector3& scale) {
    m_mesh_scale = scale;
    // Scale is applied every frame in _process()
}

void OhaoMeshInstance::set_metallic(float metallic) {
    m_metallic = metallic;
    m_material_dirty = true;
}

void OhaoMeshInstance::set_roughness(float roughness) {
    m_roughness = roughness;
    m_material_dirty = true;
}

void OhaoMeshInstance::set_actor_name_override(const String& name) {
    m_actor_name_override = name;
}

} // namespace godot
