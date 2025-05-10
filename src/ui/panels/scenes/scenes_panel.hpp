#pragma once

#include <string>
#include <vector>
#include <memory>
#include "renderer/vulkan_context.hpp"
#include "ui/common/panel_base.hpp"

namespace ohao {

class ScenesPanel : public PanelBase {
public:
    ScenesPanel();
    ~ScenesPanel() = default;

    void render() override;
    void setVulkanContext(VulkanContext* context) { m_context = context; }

private:
    VulkanContext* m_context = nullptr;
    std::string m_newSceneName;
    std::string m_sceneToLoad;
    std::string m_sceneToSave;
    std::string m_projectPath;
    bool m_showCreateDialog = false;
    bool m_showLoadDialog = false;
    bool m_showSaveDialog = false;
    bool m_showConfirmClose = false;
    bool m_showSaveDirtyDialog = false;
    std::string m_sceneToClose;
    std::string m_pendingSceneToActivate; // Used when we need to save before activating a new scene

    void renderCreateSceneDialog();
    void renderLoadSceneDialog();
    void renderSaveSceneDialog();
    void renderConfirmCloseDialog();
    void renderSaveDirtySceneDialog();
    
    bool createNewScene(const std::string& name);
    bool saveCurrentScene();
    bool tryActivateScene(const std::string& name);
    bool tryCloseScene(const std::string& name);
    
    // Project management
    bool loadProject(const std::string& path);
    bool saveProject();
    bool createNewProject(const std::string& name);
};

} // namespace ohao 