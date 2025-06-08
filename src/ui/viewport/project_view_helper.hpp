/**
 * @file project_view_helper.hpp
 * @brief Helper class for managing project-related viewport operations
 */
#pragma once

#include <string>
#include <vector>
#include "renderer/vulkan_context.hpp"

namespace ohao {

class SceneViewport;

/**
 * @class ProjectViewHelper
 * @brief Helper class for handling project-related viewport operations
 */
class ProjectViewHelper {
public:
    explicit ProjectViewHelper(SceneViewport* viewport);
    ~ProjectViewHelper() = default;
    
    void loadRecentProjects();
    bool createNewProject(VulkanContext* context, const std::string& projectDir = "");
    bool loadProject(VulkanContext* context, const std::string& projectPath = "");
    bool renderStartupDialog(VulkanContext* context);
    std::string getEngineConfigPath() const;

private:
    SceneViewport* m_viewport;
    bool m_showStartupDialog;
    std::vector<std::string> m_recentProjects;
};

} // namespace ohao 