#include "scene_manager.hpp"
#include "scene.hpp"
#include "../serialization/scene_serializer.hpp"
#include <iostream>
#include <filesystem>

namespace ohao {

SceneManager::SceneManager()
    : hasUnsavedChangesFlag(false)
{
}

SceneManager::~SceneManager() = default;

Scene::Ptr SceneManager::createScene(const std::string& name) {
    // Check if there's already a scene with this name
    if (getScene(name)) {
        std::cerr << "Scene with name '" << name << "' already exists" << std::endl;
        return nullptr;
    }
    
    // Create new scene
    auto scene = Scene::create(name);
    if (!scene) {
        std::cerr << "Failed to create scene: " << name << std::endl;
        return nullptr;
    }
    
    // Add to map
    scenes[name] = scene;
    
    // Set as active scene if this is the first scene
    if (!activeScene) {
        activeScene = scene;
    }
    
    notifySceneChanged(name);
    return scene;
}

Scene::Ptr SceneManager::createDefaultScene() {
    // Generate unique name
    std::string name = "DefaultScene";
    int counter = 1;
    
    while (getScene(name)) {
        name = "DefaultScene_" + std::to_string(counter++);
    }
    
    // Create scene
    auto scene = Scene::create(name);
    if (!scene) {
        std::cerr << "Failed to create default scene" << std::endl;
        return nullptr;
    }
    
    // Add to map
    scenes[name] = scene;
    
    // Set as active scene if this is the first scene
    if (!activeScene) {
        activeScene = scene;
    }
    
    notifySceneChanged(name);
    return scene;
}

void SceneManager::removeScene(const std::string& name) {
    // Find scene in map
    auto it = scenes.find(name);
    if (it == scenes.end()) {
        return;
    }
    
    // If this is the active scene, clear the active scene pointer
    if (activeScene == it->second) {
        activeScene = nullptr;
        
        // Try to set another scene as active
        if (!scenes.empty()) {
            auto firstIt = scenes.begin();
            if (firstIt->first != name) { // Avoid the one we're removing
                activeScene = firstIt->second;
            } else if (scenes.size() > 1) {
                // Find the next one after the one we're removing
                activeScene = (++firstIt)->second;
            }
        }
    }
    
    // Remove from map
    scenes.erase(it);
    
    notifySceneChanged(name);
}

Scene::Ptr SceneManager::getScene(const std::string& name) {
    auto it = scenes.find(name);
    return (it != scenes.end()) ? it->second : nullptr;
}

void SceneManager::setActiveScene(const std::string& name) {
    auto scene = getScene(name);
    if (scene) {
        activeScene = scene;
        notifySceneChanged(name);
    }
}

std::vector<Scene::Ptr> SceneManager::getAllScenes() const {
    std::vector<Scene::Ptr> result;
    result.reserve(scenes.size());
    
    for (const auto& [name, scene] : scenes) {
        result.push_back(scene);
    }
    
    return result;
}

std::vector<std::string> SceneManager::getSceneNames() const {
    std::vector<std::string> result;
    result.reserve(scenes.size());
    
    for (const auto& [name, _] : scenes) {
        result.push_back(name);
    }
    
    return result;
}

void SceneManager::saveScene(const std::string& name) {
    auto scene = getScene(name);
    if (!scene) {
        return;
    }
    
    // Get project path from scene
    std::string projectPath = scene->getProjectPath();
    if (projectPath.empty()) {
        // Default to current directory if not set
        projectPath = ".";
    }
    
    // Create scenes directory if it doesn't exist
    std::filesystem::path scenesPath = std::filesystem::path(projectPath) / "scenes";
    if (!std::filesystem::exists(scenesPath)) {
        std::filesystem::create_directories(scenesPath);
    }
    
    // Build full path
    std::string fullPath = (scenesPath / (name + Scene::FILE_EXTENSION)).string();
    
    // Save the scene
    if (scene->saveToFile(fullPath)) {
        std::cout << "Scene saved to: " << fullPath << std::endl;
    } else {
        std::cerr << "Failed to save scene: " << fullPath << std::endl;
    }
}

void SceneManager::loadScene(const std::string& filename) {
    // Get scene name from file name (without extension)
    std::string name = std::filesystem::path(filename).stem().string();
    
    // Check if a scene with this name is already loaded
    if (getScene(name)) {
        std::cerr << "Scene with name '" << name << "' already loaded" << std::endl;
        return;
    }
    
    // Create new scene
    auto scene = Scene::create(name);
    if (!scene) {
        std::cerr << "Failed to create scene for loading: " << name << std::endl;
        return;
    }
    
    // Load the scene
    SceneSerializer serializer(scene.get());
    if (!serializer.deserialize(filename)) {
        std::cerr << "Failed to load scene from file: " << filename << std::endl;
        return;
    }
    
    // Add to scenes
    scenes[name] = scene;
    
    // Set as active scene if no active scene
    if (!activeScene) {
        activeScene = scene;
    }
    
    notifySceneChanged(name);
}

void SceneManager::saveAllScenes() {
    for (const auto& [name, scene] : scenes) {
        saveScene(name);
    }
}

bool SceneManager::hasUnsavedChanges() const {
    return hasUnsavedChangesFlag;
}

void SceneManager::updateUnsavedChangesFlag() {
    hasUnsavedChangesFlag = false;
    
    // Check if any scene is dirty
    for (const auto& [_, scene] : scenes) {
        if (scene->isDirty()) {
            hasUnsavedChangesFlag = true;
            break;
        }
    }
}

void SceneManager::notifySceneChanged(const std::string& sceneName) {
    // Update flags
    updateUnsavedChangesFlag();
    
    // Notify callbacks
    for (const auto& callback : sceneChangeCallbacks) {
        callback(sceneName);
    }
}

void SceneManager::registerSceneChangeCallback(SceneChangeCallback callback) {
    sceneChangeCallbacks.push_back(callback);
}

void SceneManager::unregisterSceneChangeCallback(SceneChangeCallback callback) {
    // Instead of using std::remove (which would require operator==),
    // use remove_if with a way to compare the function objects
    // Note: this is a best-effort approach. std::function objects are difficult to compare.
    auto it = std::remove_if(sceneChangeCallbacks.begin(), sceneChangeCallbacks.end(),
        [&callback](const SceneChangeCallback& cb) {
            // Try to compare the target types - this is an approximate solution
            return cb.target_type() == callback.target_type();
        });
    
    if (it != sceneChangeCallbacks.end()) {
        sceneChangeCallbacks.erase(it, sceneChangeCallbacks.end());
    }
}

// Scene change tracking methods
void SceneManager::beginSceneModification() {
    if (activeScene) {
        activeScene->beginModification();
    }
}

void SceneManager::endSceneModification() {
    if (activeScene) {
        activeScene->endModification();
    }
    
    updateUnsavedChangesFlag();
}

bool SceneManager::canUndo() const {
    return activeScene && activeScene->canUndo();
}

bool SceneManager::canRedo() const {
    return activeScene && activeScene->canRedo();
}

void SceneManager::undo() {
    if (activeScene) {
        activeScene->undo();
        updateUnsavedChangesFlag();
    }
}

void SceneManager::redo() {
    if (activeScene) {
        activeScene->redo();
        updateUnsavedChangesFlag();
    }
}

void SceneManager::clearHistory() {
    if (activeScene) {
        activeScene->clearHistory();
    }
}

void SceneManager::saveHistory(const std::string& filename) const {
    if (activeScene) {
        activeScene->saveHistory(filename);
    }
}

void SceneManager::loadHistory(const std::string& filename) {
    if (activeScene) {
        activeScene->loadHistory(filename);
        updateUnsavedChangesFlag();
    }
}

} // namespace ohao 