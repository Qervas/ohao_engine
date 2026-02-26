#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <memory>

// Forward declare OHAO types
namespace ohao {
    class Scene;
    class Actor;
    class OffscreenRenderer;
    class MaterialComponent;
}

namespace godot {

class OhaoViewport;

/**
 * OhaoMeshInstance - Self-managing visual actor node
 *
 * Creates an OHAO mesh actor on _ready(), syncs Godot transform -> OHAO
 * every frame in _process(), and removes the actor on EXIT_TREE.
 *
 * Attach as a child of any Node3D. The OHAO actor name defaults to the
 * parent node's name (required for weapon hitscan matching).
 *
 * Sync direction: Godot -> OHAO (opposite of OhaoPhysicsBody).
 */
class OhaoMeshInstance : public Node3D {
    GDCLASS(OhaoMeshInstance, Node3D)

public:
    enum MeshType {
        MESH_CUBE = 0,
        MESH_SPHERE = 1,
        MESH_CYLINDER = 2,
        MESH_PLANE = 3
    };

private:
    // Exported properties
    MeshType m_mesh_type = MESH_CUBE;
    Color m_mesh_color = Color(0.7f, 0.7f, 0.8f);
    Vector3 m_mesh_scale = Vector3(1, 1, 1);
    float m_metallic = 0.0f;
    float m_roughness = 0.5f;
    String m_actor_name_override;

    // OHAO engine references
    OhaoViewport* m_viewport = nullptr;
    ohao::Scene* m_scene = nullptr;
    ohao::OffscreenRenderer* m_renderer = nullptr;
    std::shared_ptr<ohao::Actor> m_actor;
    bool m_synced = false;

    // Dirty flags for material changes after creation
    bool m_material_dirty = false;

    // Internal helpers
    OhaoViewport* find_viewport() const;
    void create_ohao_actor();
    void destroy_ohao_actor();
    String resolve_actor_name() const;

protected:
    static void _bind_methods();

public:
    OhaoMeshInstance();
    ~OhaoMeshInstance();

    void _ready() override;
    void _process(double delta) override;
    void _notification(int p_what);

    // Mesh type
    void set_mesh_type(int type);
    int get_mesh_type() const { return m_mesh_type; }

    // Color
    void set_mesh_color(const Color& color);
    Color get_mesh_color() const { return m_mesh_color; }

    // Scale
    void set_mesh_scale(const Vector3& scale);
    Vector3 get_mesh_scale() const { return m_mesh_scale; }

    // PBR
    void set_metallic(float metallic);
    float get_metallic() const { return m_metallic; }
    void set_roughness(float roughness);
    float get_roughness() const { return m_roughness; }

    // Actor name override
    void set_actor_name_override(const String& name);
    String get_actor_name_override() const { return m_actor_name_override; }

    // Runtime queries
    bool is_synced() const { return m_synced; }
    String get_resolved_actor_name() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::OhaoMeshInstance::MeshType);
