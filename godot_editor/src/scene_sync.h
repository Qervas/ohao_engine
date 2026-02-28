#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>

#include <string>

// Forward declare OHAO types
namespace ohao {
    class OffscreenRenderer;
    class Scene;
    class Camera;
}

namespace godot {

/**
 * SceneSync - Godot <-> OHAO scene synchronization bridge
 *
 * Handles syncing Godot's scene tree to OHAO's internal scene representation,
 * plus the add_cube/sphere/plane/cylinder/light API for GDScript scene building.
 */
class SceneSync {
public:
    SceneSync();

    // Sync entire Godot scene tree to OHAO scene
    // Returns the number of synced objects, or -1 if nothing to sync
    int syncFromGodot(Node* root_node, ohao::Scene* scene, ohao::OffscreenRenderer* renderer);

    // Load a .tscn file into OHAO scene
    void loadTscn(const String& path, ohao::Scene* scene, ohao::OffscreenRenderer* renderer);

    // Clear all actors from scene
    void clearScene(ohao::Scene* scene);

    // Scene building API (called from GDScript via OhaoViewport)
    void addCube(ohao::Scene* scene, const String& name, const Vector3& position,
                 const Vector3& rotation, const Vector3& scale, const Color& color);
    void addSphere(ohao::Scene* scene, const String& name, const Vector3& position,
                   const Vector3& rotation, const Vector3& scale, const Color& color);
    void addPlane(ohao::Scene* scene, const String& name, const Vector3& position,
                  const Vector3& rotation, const Vector3& scale, const Color& color);
    void addCylinder(ohao::Scene* scene, const String& name, const Vector3& position,
                     const Vector3& rotation, const Vector3& scale, const Color& color);
    void addDirectionalLight(ohao::Scene* scene, const String& name, const Vector3& position,
                             const Vector3& direction, const Color& color, float intensity);
    void addPointLight(ohao::Scene* scene, const String& name, const Vector3& position,
                       const Color& color, float intensity, float range);

    // Import a model file (OBJ/GLTF/GLB) into the scene
    bool importModel(ohao::Scene* scene, ohao::OffscreenRenderer* renderer, const std::string& filepath);

    // Rebuild GPU buffers after adding objects
    void finishSync(ohao::OffscreenRenderer* renderer);

    // Get count of objects synced in last syncFromGodot call
    int getSyncedObjectCount() const { return m_synced_object_count; }

    // Resolve res:// path to absolute filesystem path
    static std::string resolveResPath(const String& res_path);

private:
    void traverseAndSync(Node* node, ohao::Scene* scene, ohao::OffscreenRenderer* renderer);
    void countSyncableObjects(Node* node);

    // Shared implementation for addCube/addSphere/addPlane/addCylinder
    void addPrimitive(ohao::Scene* scene, int primitiveType, const String& name,
                      const Vector3& position, const Vector3& rotation,
                      const Vector3& scale, const Color& color);

    int m_synced_object_count = 0;
};

} // namespace godot
