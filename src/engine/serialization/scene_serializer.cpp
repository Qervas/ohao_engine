#include "scene_serializer.hpp"
#include "actor_serializer.hpp"
#include "engine/scene/scene.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <ctime>
#include <iomanip>

namespace ohao {

SceneSerializer::SceneSerializer(Scene* scene)
    : m_scene(scene)
{
}

bool SceneSerializer::serialize(const std::string& filePath) {
    if (!m_scene) {
        std::cerr << "SceneSerializer: Cannot serialize null scene" << std::endl;
        return false;
    }
    
    try {
        // Update scene descriptor
        SceneDescriptor descriptor = m_scene->getDescriptor();
        descriptor.name = m_scene->getName();
        descriptor.lastModified = std::to_string(std::time(nullptr));
        m_scene->setDescriptor(descriptor);
        
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
        sceneJson["name"] = m_scene->getName();
        
        // Add actors
        nlohmann::json actorsJson = nlohmann::json::array();
        
        // Serialize each actor (excluding the root node)
        for (const auto& [id, actor] : m_scene->getAllActors()) {
            if (actor != m_scene->getRootNode()) {
                actorsJson.push_back(ActorSerializer::serializeActor(actor.get()));
            }
        }
        
        sceneJson["actors"] = actorsJson;
        
        // Determine proper file path
        std::filesystem::path outPath(filePath);
        if (outPath.extension().empty()) {
            outPath += Scene::FILE_EXTENSION;
        }
        
        // Create directory if it doesn't exist
        std::filesystem::path directory = outPath.parent_path();
        if (!directory.empty() && !std::filesystem::exists(directory)) {
            std::filesystem::create_directories(directory);
        }
        
        // Write JSON to file
        std::ofstream file(outPath);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for writing: " << outPath << std::endl;
            return false;
        }
        
        file << std::setw(4) << sceneJson << std::endl;
        file.close();
        
        std::cout << "Scene saved to: " << outPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving scene: " << e.what() << std::endl;
        return false;
    }
}

bool SceneSerializer::deserialize(const std::string& filePath) {
    if (!m_scene) {
        std::cerr << "SceneSerializer: Cannot deserialize to null scene" << std::endl;
        return false;
    }
    
    try {
        // Resolve path
        std::filesystem::path path(filePath);
        if (!std::filesystem::exists(path)) {
            std::cerr << "File not found: " << path << std::endl;
            return false;
        }
        
        // Read JSON from file
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open file for reading: " << path << std::endl;
            return false;
        }
        
        nlohmann::json sceneJson;
        file >> sceneJson;
        file.close();
        
        // Set project path based on file location
        m_scene->setProjectPath(path.parent_path().string());
        
        // Clear existing scene data
        m_scene->removeAllActors();
        
        // Load scene properties
        if (sceneJson.contains("name")) {
            m_scene->setName(sceneJson["name"].get<std::string>());
        }
        
        // Load descriptor
        SceneDescriptor descriptor;
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
            
            m_scene->setDescriptor(descriptor);
        }
        
        // Load actors
        if (sceneJson.contains("actors") && sceneJson["actors"].is_array()) {
            // First pass: create all actors with basic properties
            std::unordered_map<uint64_t, Actor::Ptr> actorsById;
            
            for (const auto& actorJson : sceneJson["actors"]) {
                auto actor = ActorSerializer::deserializeActor(actorJson);
                if (actor) {
                    m_scene->addActor(actor);
                    
                    // Store for second pass
                    if (actorJson.contains("id")) {
                        uint64_t id = actorJson["id"].get<uint64_t>();
                        actorsById[id] = actor;
                    }
                }
            }
            
            // Second pass: resolve parent-child relationships
            for (const auto& actorJson : sceneJson["actors"]) {
                if (actorJson.contains("id") && actorJson.contains("parentId")) {
                    uint64_t id = actorJson["id"].get<uint64_t>();
                    uint64_t parentId = actorJson["parentId"].get<uint64_t>();
                    
                    if (parentId > 0 && actorsById.find(id) != actorsById.end()) {
                        auto actor = actorsById[id];
                        auto parent = m_scene->findActor(parentId);
                        if (parent) {
                            actor->setParent(parent.get());
                        }
                    }
                }
            }
        }
        
        // Update scene buffers (important: this makes sure objects are rendered properly)
        m_scene->updateSceneBuffers();
        
        std::cout << "Scene loaded from: " << path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading scene: " << e.what() << std::endl;
        return false;
    }
}

} // namespace ohao 