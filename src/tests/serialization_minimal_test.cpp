#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

// Define a minimal scene descriptor
struct SceneDescriptor {
    std::string name;
    std::string version = "1.0";
    std::vector<std::string> tags;
    std::string createdBy;
    std::string lastModified;
    std::unordered_map<std::string, std::string> metadata;
};

// Define a minimal light struct
struct Light {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    bool enabled{true};
};

// Define a minimal transform struct
struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
};

// Define a minimal component struct
struct Component {
    std::string type;
    bool enabled = true;
};

// Define a minimal actor struct
struct Actor {
    uint64_t id;
    std::string name;
    bool active = true;
    uint64_t parentId = 0;
    Transform transform;
    std::vector<Component> components;
};

// Serialize a scene to JSON
bool serializeScene(const std::string& filePath, 
                   const std::string& sceneName, 
                   const SceneDescriptor& descriptor,
                   const std::vector<Actor>& actors,
                   const std::unordered_map<std::string, Light>& lights) {
    try {
        // Create JSON object
        nlohmann::json sceneJson;
        
        // Add scene descriptor
        nlohmann::json descriptorJson;
        descriptorJson["name"] = descriptor.name;
        descriptorJson["version"] = descriptor.version;
        descriptorJson["tags"] = descriptor.tags;
        descriptorJson["createdBy"] = descriptor.createdBy;
        descriptorJson["lastModified"] = descriptor.lastModified;
        descriptorJson["metadata"] = descriptor.metadata;
        
        sceneJson["descriptor"] = descriptorJson;
        sceneJson["name"] = sceneName;
        
        // Add actors
        nlohmann::json actorsJson = nlohmann::json::array();
        for (const auto& actor : actors) {
            nlohmann::json actorJson;
            actorJson["id"] = actor.id;
            actorJson["name"] = actor.name;
            actorJson["active"] = actor.active;
            actorJson["parentId"] = actor.parentId;
            
            // Add transform
            nlohmann::json transformJson;
            transformJson["position"] = {actor.transform.position.x, actor.transform.position.y, actor.transform.position.z};
            transformJson["rotation"] = {actor.transform.rotation.x, actor.transform.rotation.y, actor.transform.rotation.z};
            transformJson["scale"] = {actor.transform.scale.x, actor.transform.scale.y, actor.transform.scale.z};
            actorJson["transform"] = transformJson;
            
            // Add components
            nlohmann::json componentsJson = nlohmann::json::array();
            for (const auto& component : actor.components) {
                nlohmann::json componentJson;
                componentJson["type"] = component.type;
                
                if (component.type == "MeshComponent") {
                    nlohmann::json meshJson;
                    meshJson["enabled"] = component.enabled;
                    componentJson["mesh"] = meshJson;
                }
                
                componentsJson.push_back(componentJson);
            }
            actorJson["components"] = componentsJson;
            
            actorsJson.push_back(actorJson);
        }
        sceneJson["actors"] = actorsJson;
        
        // Add lights
        nlohmann::json lightsJson = nlohmann::json::object();
        for (const auto& [name, light] : lights) {
            nlohmann::json lightJson;
            lightJson["position"] = {light.position.x, light.position.y, light.position.z};
            lightJson["color"] = {light.color.r, light.color.g, light.color.b};
            lightJson["intensity"] = light.intensity;
            lightJson["enabled"] = light.enabled;
            
            lightsJson[name] = lightJson;
        }
        
        sceneJson["lights"] = lightsJson;
        
        // Write JSON to file
        std::ofstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for writing: " << filePath << std::endl;
            return false;
        }
        
        file << sceneJson.dump(4) << std::endl;
        file.close();
        
        std::cout << "Scene saved to: " << filePath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving scene: " << e.what() << std::endl;
        return false;
    }
}

// Deserialize a scene from JSON
bool deserializeScene(const std::string& filePath,
                     std::string& sceneName,
                     SceneDescriptor& descriptor,
                     std::vector<Actor>& actors,
                     std::unordered_map<std::string, Light>& lights) {
    try {
        // Read JSON from file
        std::ifstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for reading: " << filePath << std::endl;
            return false;
        }
        
        nlohmann::json sceneJson;
        file >> sceneJson;
        file.close();
        
        // Clear existing data
        actors.clear();
        lights.clear();
        
        // Load scene properties
        if (sceneJson.contains("name")) {
            sceneName = sceneJson["name"].get<std::string>();
        }
        
        // Load descriptor
        if (sceneJson.contains("descriptor")) {
            const auto& descriptorJson = sceneJson["descriptor"];
            
            if (descriptorJson.contains("name")) {
                descriptor.name = descriptorJson["name"].get<std::string>();
            }
            
            if (descriptorJson.contains("version")) {
                descriptor.version = descriptorJson["version"].get<std::string>();
            }
            
            if (descriptorJson.contains("tags")) {
                descriptor.tags = descriptorJson["tags"].get<std::vector<std::string>>();
            }
            
            if (descriptorJson.contains("createdBy")) {
                descriptor.createdBy = descriptorJson["createdBy"].get<std::string>();
            }
            
            if (descriptorJson.contains("lastModified")) {
                descriptor.lastModified = descriptorJson["lastModified"].get<std::string>();
            }
            
            if (descriptorJson.contains("metadata")) {
                descriptor.metadata = descriptorJson["metadata"].get<std::unordered_map<std::string, std::string>>();
            }
        }
        
        // Load actors
        if (sceneJson.contains("actors") && sceneJson["actors"].is_array()) {
            for (const auto& actorJson : sceneJson["actors"]) {
                Actor actor;
                
                if (actorJson.contains("id")) {
                    actor.id = actorJson["id"].get<uint64_t>();
                }
                
                if (actorJson.contains("name")) {
                    actor.name = actorJson["name"].get<std::string>();
                }
                
                if (actorJson.contains("active")) {
                    actor.active = actorJson["active"].get<bool>();
                }
                
                if (actorJson.contains("parentId")) {
                    actor.parentId = actorJson["parentId"].get<uint64_t>();
                }
                
                // Load transform
                if (actorJson.contains("transform")) {
                    const auto& transformJson = actorJson["transform"];
                    
                    if (transformJson.contains("position") && 
                        transformJson["position"].is_array() && 
                        transformJson["position"].size() == 3) {
                        actor.transform.position = glm::vec3(
                            transformJson["position"][0].get<float>(),
                            transformJson["position"][1].get<float>(),
                            transformJson["position"][2].get<float>()
                        );
                    }
                    
                    if (transformJson.contains("rotation") && 
                        transformJson["rotation"].is_array() && 
                        transformJson["rotation"].size() == 3) {
                        actor.transform.rotation = glm::vec3(
                            transformJson["rotation"][0].get<float>(),
                            transformJson["rotation"][1].get<float>(),
                            transformJson["rotation"][2].get<float>()
                        );
                    }
                    
                    if (transformJson.contains("scale") && 
                        transformJson["scale"].is_array() && 
                        transformJson["scale"].size() == 3) {
                        actor.transform.scale = glm::vec3(
                            transformJson["scale"][0].get<float>(),
                            transformJson["scale"][1].get<float>(),
                            transformJson["scale"][2].get<float>()
                        );
                    }
                }
                
                // Load components
                if (actorJson.contains("components") && actorJson["components"].is_array()) {
                    for (const auto& componentJson : actorJson["components"]) {
                        Component component;
                        
                        if (componentJson.contains("type")) {
                            component.type = componentJson["type"].get<std::string>();
                            
                            if (component.type == "MeshComponent" && 
                                componentJson.contains("mesh") && 
                                componentJson["mesh"].contains("enabled")) {
                                component.enabled = componentJson["mesh"]["enabled"].get<bool>();
                            }
                        }
                        
                        actor.components.push_back(component);
                    }
                }
                
                actors.push_back(actor);
            }
        }
        
        // Load lights
        if (sceneJson.contains("lights") && sceneJson["lights"].is_object()) {
            for (auto it = sceneJson["lights"].begin(); it != sceneJson["lights"].end(); ++it) {
                std::string lightName = it.key();
                const auto& lightJson = it.value();
                
                Light light;
                
                if (lightJson.contains("position") && lightJson["position"].is_array() && 
                    lightJson["position"].size() == 3) {
                    light.position = glm::vec3(
                        lightJson["position"][0].get<float>(),
                        lightJson["position"][1].get<float>(),
                        lightJson["position"][2].get<float>()
                    );
                }
                
                if (lightJson.contains("color") && lightJson["color"].is_array() && 
                    lightJson["color"].size() == 3) {
                    light.color = glm::vec3(
                        lightJson["color"][0].get<float>(),
                        lightJson["color"][1].get<float>(),
                        lightJson["color"][2].get<float>()
                    );
                }
                
                if (lightJson.contains("intensity")) {
                    light.intensity = lightJson["intensity"].get<float>();
                }
                
                if (lightJson.contains("enabled")) {
                    light.enabled = lightJson["enabled"].get<bool>();
                }
                
                lights[lightName] = light;
            }
        }
        
        std::cout << "Scene loaded from: " << filePath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading scene: " << e.what() << std::endl;
        return false;
    }
}

int main() {
    std::cout << "Testing Minimal Scene Serialization/Deserialization" << std::endl;
    
    const std::string scenePath = "test_scenes/minimal_test_scene.ohscene";
    
    // Create test data
    std::string sceneName = "Test Scene";
    
    SceneDescriptor descriptor;
    descriptor.name = "Test Scene";
    descriptor.version = "1.0";
    descriptor.tags = {"test", "minimal", "serialization"};
    descriptor.createdBy = "Minimal Serialization Test";
    descriptor.lastModified = "1234567890";
    descriptor.metadata["environment"] = "test";
    descriptor.metadata["author"] = "OHAO Engine";
    
    // Create some actors
    std::vector<Actor> actors;
    
    // Add a cube actor
    Actor cube;
    cube.id = 2;
    cube.name = "Cube";
    cube.active = true;
    cube.parentId = 0;
    cube.transform.position = glm::vec3(0.0f, 0.0f, 0.0f);
    cube.transform.rotation = glm::vec3(0.0f, 0.0f, 0.0f);
    cube.transform.scale = glm::vec3(1.0f, 1.0f, 1.0f);
    
    // Add a mesh component to the cube
    Component meshComponent;
    meshComponent.type = "MeshComponent";
    meshComponent.enabled = true;
    cube.components.push_back(meshComponent);
    
    actors.push_back(cube);
    
    // Add lights
    std::unordered_map<std::string, Light> lights;
    
    Light mainLight;
    mainLight.position = glm::vec3(0.0f, 5.0f, 0.0f);
    mainLight.color = glm::vec3(1.0f, 1.0f, 1.0f);
    mainLight.intensity = 1.0f;
    lights["DefaultLight"] = mainLight;
    
    // Save to file
    if (!serializeScene(scenePath, sceneName, descriptor, actors, lights)) {
        std::cerr << "Failed to serialize scene!" << std::endl;
        return 1;
    }
    
    // Clear data for loading test
    std::string loadedSceneName;
    SceneDescriptor loadedDescriptor;
    std::vector<Actor> loadedActors;
    std::unordered_map<std::string, Light> loadedLights;
    
    // Load from file
    if (!deserializeScene(scenePath, loadedSceneName, loadedDescriptor, loadedActors, loadedLights)) {
        std::cerr << "Failed to deserialize scene!" << std::endl;
        return 1;
    }
    
    // Verify data
    std::cout << "\nVerifying loaded data:" << std::endl;
    std::cout << "Scene name: " << loadedSceneName << std::endl;
    std::cout << "Descriptor name: " << loadedDescriptor.name << std::endl;
    std::cout << "Number of actors: " << loadedActors.size() << std::endl;
    std::cout << "Number of lights: " << loadedLights.size() << std::endl;
    
    if (!loadedActors.empty()) {
        const auto& actor = loadedActors[0];
        std::cout << "Actor name: " << actor.name << std::endl;
        std::cout << "Actor position: " << actor.transform.position.x << ", " 
                  << actor.transform.position.y << ", " 
                  << actor.transform.position.z << std::endl;
        std::cout << "Actor has " << actor.components.size() << " components" << std::endl;
    }
    
    if (loadedLights.find("DefaultLight") != loadedLights.end()) {
        const auto& light = loadedLights["DefaultLight"];
        std::cout << "DefaultLight position: " << light.position.x << ", " 
                  << light.position.y << ", " << light.position.z << std::endl;
        std::cout << "DefaultLight intensity: " << light.intensity << std::endl;
    }
    
    std::cout << "\nTest completed successfully!" << std::endl;
    return 0;
} 