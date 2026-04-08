#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace ohao {

class Scene;

/**
 * SceneSerializer - Save/load scenes as JSON
 *
 * Serializes actors, transforms, meshes, materials, lights, and physics
 * to a portable JSON format. AI agents use this to save/restore state
 * and experiment with scene modifications.
 *
 * Format:
 * {
 *   "name": "MyScene",
 *   "version": "1.0",
 *   "actors": [
 *     {
 *       "name": "Box1",
 *       "id": 12345,
 *       "transform": { "position": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1] },
 *       "mesh": { "primitive": "cube" },
 *       "material": { "baseColor": [0.8, 0.2, 0.2] },
 *       "light": { "type": "point", "color": [1,1,1], "intensity": 1.0, "range": 10.0 },
 *       "physics": { "bodyType": "dynamic", "mass": 1.0, "friction": 0.5, "restitution": 0.3 }
 *     }
 *   ]
 * }
 */
class SceneSerializer {
public:
    // Serialize a scene to JSON
    static nlohmann::json serialize(const Scene* scene);

    // Deserialize JSON into a scene (clears existing actors)
    static bool deserialize(Scene* scene, const nlohmann::json& json);

    // File I/O convenience
    static bool saveToFile(const Scene* scene, const std::string& filepath);
    static bool loadFromFile(Scene* scene, const std::string& filepath);

    // Get last error message
    static const std::string& getLastError();

private:
    static std::string s_lastError;
};

} // namespace ohao
