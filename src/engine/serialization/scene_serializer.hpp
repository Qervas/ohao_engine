#pragma once

#include <string>
#include <memory>

namespace ohao {

class Scene;

/**
 * @class SceneSerializer
 * @brief Handles serialization and deserialization of Scene objects to and from files.
 * 
 * This class provides functionality to save a Scene to a file in JSON format and
 * load a Scene from a JSON file.
 */
class SceneSerializer {
public:
    /**
     * @brief Construct a new Scene Serializer object
     * @param scene The scene to be serialized/deserialized
     */
    explicit SceneSerializer(Scene* scene);
    
    /**
     * @brief Serialize the scene to a JSON file
     * @param filePath The path where the scene file will be saved
     * @return true if saving was successful, false otherwise
     */
    bool serialize(const std::string& filePath);
    
    /**
     * @brief Deserialize a scene from a JSON file
     * @param filePath The path to the scene file
     * @return true if loading was successful, false otherwise
     */
    bool deserialize(const std::string& filePath);
    
private:
    Scene* m_scene;
};

} // namespace ohao 