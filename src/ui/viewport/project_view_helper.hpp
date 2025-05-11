#pragma once

#include <string>
#include <vector>
#include <memory>
#include "imgui.h"
#include "renderer/vulkan_context.hpp"

namespace ohao {

class SceneViewport;

class ProjectViewHelper {
public:
    ProjectViewHelper(SceneViewport* viewport);
    ~ProjectViewHelper() = default;
    
    struct RecentProject {
        std::string name;
        std::string path;
        std::string lastOpened; // Timestamp for sorting
    };

    // Dialogs
    bool renderStartupDialog(VulkanContext* context);
    bool renderCreateProjectDialog(VulkanContext* context);
    bool renderSaveProjectDialog(VulkanContext* context, bool forceSaveAs);
    
    // Project operations
    bool createNewProject(VulkanContext* context, const std::string& projectDir = "");
    bool saveProject(VulkanContext* context, bool forceSaveAs = false);
    bool loadProject(VulkanContext* context, const std::string& projectPath = "");
    void closeProject(VulkanContext* context);
    
    // Recent projects
    void loadRecentProjects();
    void saveRecentProjects();
    void addToRecentProjects(const std::string& projectPath);
    
    // Getters
    const std::string& getProjectPath() const { return m_projectPath; }
    const std::string& getProjectDir() const { return m_projectDir; }
    const std::string& getProjectName() const { return m_projectName; }
    bool hasProjectPath() const { return !m_projectPath.empty(); }
    bool isShowingStartupDialog() const { return m_showStartupDialog; }
    
    // Setters
    void setProjectPath(const std::string& path) { m_projectPath = path; }
    void setProjectDir(const std::string& dir) { m_projectDir = dir; }
    void setProjectName(const std::string& name) { m_projectName = name; }
    void showStartupDialog(bool show) { m_showStartupDialog = show; }
    
private:
    SceneViewport* m_viewport; // Back-reference to the parent viewport
    
    std::string m_projectPath;
    std::string m_projectDir;
    std::string m_projectName;
    std::vector<RecentProject> m_recentProjects;
    const size_t MAX_RECENT_PROJECTS = 10;
    bool m_showStartupDialog = true;
    
    // Helpers
    std::string getEngineConfigPath() const;
    void createProjectDirectories(const std::string& rootDir);
};

} // namespace ohao 