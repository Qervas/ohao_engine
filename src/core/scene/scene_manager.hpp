#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "scene.hpp"

namespace ohao {

class SceneManager {
public:
    SceneManager();
    ~SceneManager();

    // Scene management
    Scene::Ptr createScene(const std::string& name = "New Scene");
    Scene::Ptr createDefaultScene();
    void removeScene(const std::string& name);
    Scene::Ptr getScene(const std::string& name);
    Scene::Ptr getActiveScene() const { return activeScene; }
    void setActiveScene(const std::string& name);
    std::vector<Scene::Ptr> getAllScenes() const;
    std::vector<std::string> getSceneNames() const;

    // Scene state management
    bool hasUnsavedChanges() const;
    void saveScene(const std::string& name);
    void loadScene(const std::string& filename);
    void saveAllScenes();
    void loadAllScenes();

    // Scene change tracking
    void beginSceneModification();
    void endSceneModification();
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void clearHistory();
    void saveHistory(const std::string& filename) const;
    void loadHistory(const std::string& filename);

    // Scene change callbacks
    using SceneChangeCallback = std::function<void(const std::string&)>;
    void registerSceneChangeCallback(SceneChangeCallback callback);
    void unregisterSceneChangeCallback(SceneChangeCallback callback);

private:
    std::unordered_map<std::string, Scene::Ptr> scenes;
    Scene::Ptr activeScene;
    std::vector<SceneChangeCallback> sceneChangeCallbacks;
    bool hasUnsavedChangesFlag;

    void notifySceneChanged(const std::string& sceneName);
    void updateUnsavedChangesFlag();
};

} // namespace ohao 