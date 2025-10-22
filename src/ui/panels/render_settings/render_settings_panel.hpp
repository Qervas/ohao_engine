#pragma once

#include "ui/common/panel_base.hpp"

namespace ohao {

/**
 * Render Settings Panel
 * Controls rendering features like shadows, SSAO, bloom, etc.
 */
class RenderSettingsPanel : public PanelBase {
public:
    RenderSettingsPanel();
    void render() override;

private:
    // Render settings
    bool m_enableShadows = true;
    int m_shadowResolution = 2048;
    float m_shadowBias = 0.005f;
    bool m_enableSSAO = false;
    bool m_enableBloom = false;
    bool m_enableAntiAliasing = true;
    int m_msaaSamples = 4;
    bool m_enableHDR = false;
    float m_exposure = 1.0f;
};

} // namespace ohao
