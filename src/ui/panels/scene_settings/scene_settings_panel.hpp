#pragma once
#include "ui/common/panel_base.hpp"
#include "engine/scene/scene.hpp"

namespace ohao {

class SceneSettingsPanel : public PanelBase {
public:
    SceneSettingsPanel() : PanelBase("Scene Settings") {
        windowFlags = ImGuiWindowFlags_NoCollapse;
    }

    void render() override;
    void setScene(Scene* scene) { currentScene = scene; }

private:
    void renderEnvironmentSettings();
    void renderRenderSettings();
    void renderPhysicsSettings();

    Scene* currentScene{nullptr};
};

} // namespace ohao
