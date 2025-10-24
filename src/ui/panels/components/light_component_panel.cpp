#include "light_component_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <glm/gtc/type_ptr.hpp>

namespace ohao {

LightComponentPanel::LightComponentPanel() : PanelBase("Light Component") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void LightComponentPanel::render() {
    if (!visible) return;

    // Check if we're in a docked/child window context (used by SidePanelManager)
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool isInChildWindow = (window && window->ParentWindow != nullptr);

    bool shouldRenderContent = true;

    if (!isInChildWindow) {
        shouldRenderContent = ImGui::Begin(name.c_str(), &visible, windowFlags);
    }

    if (shouldRenderContent) {
        if (m_selectedActor) {
            auto lightComponent = m_selectedActor->getComponent<LightComponent>();
            if (lightComponent) {
                renderLightProperties(lightComponent.get());
            } else {
                ImGui::TextDisabled("No LightComponent found on selected actor");
            }
        } else {
            ImGui::TextDisabled("No actor selected");
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void LightComponentPanel::renderLightProperties(LightComponent* component) {
    if (!component) return;

    ImGui::Text("Light Component Properties");
    ImGui::Separator();

    // Light Type Selection
    const char* lightTypeNames[] = { "Directional", "Point", "Spot" };
    int currentType = static_cast<int>(component->getLightType());
    if (ImGui::Combo("Light Type", &currentType, lightTypeNames, 3)) {
        component->setLightType(static_cast<LightType>(currentType));
    }

    ImGui::Spacing();

    // Color Control
    glm::vec3 color = component->getColor();
    if (ImGui::ColorEdit3("Color", glm::value_ptr(color))) {
        component->setColor(color);
    }

    // Intensity Control
    float intensity = component->getIntensity();
    if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 10.0f)) {
        component->setIntensity(intensity);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Type-specific properties
    LightType lightType = component->getLightType();

    if (lightType == LightType::Point || lightType == LightType::Spot) {
        ImGui::Text("Point/Spot Light Properties:");

        // Range control for point and spot lights
        float range = component->getRange();
        if (ImGui::SliderFloat("Range", &range, 1.0f, 100.0f)) {
            component->setRange(range);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Maximum distance the light can reach");
        }
    }

    if (lightType == LightType::Directional || lightType == LightType::Spot) {
        ImGui::Text("Directional Properties:");

        // Direction control for directional and spot lights
        glm::vec3 direction = component->getDirection();
        if (renderVec3Control("Direction", direction, 0.0f)) {
            // Normalize the direction vector
            if (glm::length(direction) > 0.0f) {
                direction = glm::normalize(direction);
            } else {
                direction = glm::vec3(0.0f, -1.0f, 0.0f); // Default down direction
            }
            component->setDirection(direction);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Direction vector (will be normalized)");
        }
    }

    if (lightType == LightType::Spot) {
        ImGui::Separator();
        ImGui::Text("Spot Light Cone:");

        // Cone angle controls for spot lights
        float innerCone = component->getInnerConeAngle();
        float outerCone = component->getOuterConeAngle();

        if (ImGui::SliderFloat("Inner Cone Angle", &innerCone, 1.0f, 89.0f)) {
            // Ensure inner cone is smaller than outer cone
            if (innerCone >= outerCone) {
                innerCone = outerCone - 1.0f;
            }
            component->setInnerConeAngle(innerCone);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Inner cone angle in degrees (full intensity)");
        }

        if (ImGui::SliderFloat("Outer Cone Angle", &outerCone, 2.0f, 90.0f)) {
            // Ensure outer cone is larger than inner cone
            if (outerCone <= innerCone) {
                outerCone = innerCone + 1.0f;
            }
            component->setOuterConeAngle(outerCone);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Outer cone angle in degrees (falloff to zero)");
        }
    }

    // Light information display
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Light Information")) {
        ImGui::Text("Type: %s", lightTypeNames[currentType]);
        ImGui::Text("Color: (%.2f, %.2f, %.2f)", color.r, color.g, color.b);
        ImGui::Text("Intensity: %.2f", component->getIntensity());

        if (lightType == LightType::Point || lightType == LightType::Spot) {
            ImGui::Text("Range: %.2f", component->getRange());
        }

        if (lightType == LightType::Directional || lightType == LightType::Spot) {
            glm::vec3 dir = component->getDirection();
            ImGui::Text("Direction: (%.2f, %.2f, %.2f)", dir.x, dir.y, dir.z);
        }

        if (lightType == LightType::Spot) {
            ImGui::Text("Inner Cone: %.1f°", component->getInnerConeAngle());
            ImGui::Text("Outer Cone: %.1f°", component->getOuterConeAngle());
        }
    }
}

bool LightComponentPanel::renderVec3Control(const std::string& label, glm::vec3& values, float resetValue) {
    bool changed = false;
    ImGui::PushID(label.c_str());

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::Text("%s", label.c_str());
    ImGui::NextColumn();

    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

    float lineHeight = ImGui::GetFontSize() + GImGui->Style.FramePadding.y * 2.0f;
    ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

    // X component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    if (ImGui::Button("X", buttonSize)) {
        values.x = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##X", &values.x, 0.1f)) changed = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Y component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    if (ImGui::Button("Y", buttonSize)) {
        values.y = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##Y", &values.y, 0.1f)) changed = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Z component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    if (ImGui::Button("Z", buttonSize)) {
        values.z = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##Z", &values.z, 0.1f)) changed = true;
    ImGui::PopItemWidth();

    ImGui::Columns(1);
    ImGui::PopID();

    return changed;
}

} // namespace ohao
