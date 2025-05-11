#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "../../../core/scene/scene.hpp"
#include "../../../renderer/vulkan_context.hpp"
#include "ui/common/panel_base.hpp"

namespace ohao {

class ScenesPanel : public PanelBase {
public:
    ScenesPanel(VulkanContext* context);
    ~ScenesPanel();

    void render() override;

private:
    VulkanContext* context;
    std::string newSceneName;
    bool showNewSceneDialog;
    bool showSaveSceneDialog;
    bool showLoadSceneDialog;
    std::string selectedScene;
    std::string sceneToSave;
    std::string sceneToLoad;

    void renderSceneTabs();
    void renderSceneContent();
    void renderNewSceneDialog();
    void renderSaveSceneDialog();
    void renderLoadSceneDialog();
    void renderSceneToolbar();
    void renderUndoRedoButtons();
    void renderSceneHistory();
    
    void createNewScene();
    void saveScene(const std::string& name);
    void loadScene(const std::string& filename);
    void activateScene(const std::string& name);
    void removeScene(const std::string& name);

    void onSceneChanged(const std::string& sceneName);
};

} // namespace ohao 