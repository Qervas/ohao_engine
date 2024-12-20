#include "properties_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "scene/scene_node.hpp"
#include "vulkan_context.hpp"

namespace ohao {

PropertiesPanel::PropertiesPanel() : PanelBase("Properties") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void PropertiesPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    auto* selectedObject = SelectionManager::get().getSelectedObject();
    if (selectedObject) {
        renderNodeProperties(selectedObject);
    } else {
        ImGui::TextDisabled("No object selected");
    }

    ImGui::End();
}

void PropertiesPanel::renderNodeProperties(SceneNode* node) {
    if (!node) return;

    // Node name and type header
    char nameBuf[256];
    strcpy(nameBuf, node->getName().c_str());
    if (ImGui::InputText("##Name", nameBuf, sizeof(nameBuf))) {
        node->setName(nameBuf);
    }

    ImGui::SameLine();
    if (auto obj = asSceneObject(node)) {
        ImGui::TextDisabled("(%s)", obj->getTypeName());
    } else {
        ImGui::TextDisabled("(Node)");
    }

    ImGui::Separator();

    // Transform component (available for all nodes)
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderTransformProperties(node);
    }

    // SceneObject-specific components
    if (auto obj = asSceneObject(node)) {
        // Material component
        if (ImGui::CollapsingHeader("Material")) {
            renderMaterialProperties(obj);
        }

        // Other components
        renderComponentProperties(obj);
    }
}


void PropertiesPanel::renderTransformProperties(SceneNode* node) {
    Transform& transform = node->getTransform();

    glm::vec3 position = transform.getLocalPosition();
    glm::vec3 rotation = glm::degrees(glm::eulerAngles(transform.getLocalRotation()));
    glm::vec3 scale = transform.getLocalScale();

    renderVec3Control("Position", position);
    renderVec3Control("Rotation", rotation);
    renderVec3Control("Scale", scale, 1.0f);

    if (position != lastPosition) {
        transform.setLocalPosition(position);
        lastPosition = position;
    }

    if (rotation != lastRotation) {
        transform.setLocalRotationEuler(glm::radians(rotation));
        lastRotation = rotation;
    }

    if (scale != lastScale) {
        transform.setLocalScale(scale);
        lastScale = scale;
    }

    // Display world transform info
    if (ImGui::TreeNode("World Transform")) {
        glm::vec3 worldPos = transform.getWorldPosition();
        glm::vec3 worldRot = glm::degrees(glm::eulerAngles(transform.getWorldRotation()));
        glm::vec3 worldScale = transform.getWorldScale();

        ImGui::Text("World Position: %.2f, %.2f, %.2f", worldPos.x, worldPos.y, worldPos.z);
        ImGui::Text("World Rotation: %.2f, %.2f, %.2f", worldRot.x, worldRot.y, worldRot.z);
        ImGui::Text("World Scale: %.2f, %.2f, %.2f", worldScale.x, worldScale.y, worldScale.z);

        ImGui::TreePop();
    }
}

void PropertiesPanel::renderVec3Control(const std::string& label, glm::vec3& values,
                                      float resetValue) {
    ImGui::PushID(label.c_str());

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::Text("%s", label.c_str());
    ImGui::NextColumn();

    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

    float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
    ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

    // X component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    if (ImGui::Button("X", buttonSize))
        values.x = resetValue;
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::DragFloat("##X", &values.x, 0.1f);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Y component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    if (ImGui::Button("Y", buttonSize))
        values.y = resetValue;
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::DragFloat("##Y", &values.y, 0.1f);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Z component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    if (ImGui::Button("Z", buttonSize))
        values.z = resetValue;
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::DragFloat("##Z", &values.z, 0.1f);
    ImGui::PopItemWidth();

    ImGui::Columns(1);
    ImGui::PopID();
}

void PropertiesPanel::renderMaterialProperties(SceneObject* object) {
    if (!object) return;

    Material& material = object->getMaterial();

    // Base color
    ImGui::ColorEdit3("Base Color", &material.baseColor[0]);

    // PBR parameters
    ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f);
    ImGui::SliderFloat("AO", &material.ao, 0.0f, 1.0f);

    // Emissive
    ImGui::ColorEdit3("Emissive", &material.emissive[0]);

    // IOR
    ImGui::DragFloat("IOR", &material.ior, 0.01f, 1.0f, 3.0f);
}

void PropertiesPanel::renderComponentProperties(SceneObject* object) {
    if (!object) return;

    if (ImGui::CollapsingHeader("Model")) {
        if (auto model = object->getModel()) {
            ImGui::Text("Vertices: %zu", model->vertices.size());
            ImGui::Text("Indices: %zu", model->indices.size());
            ImGui::Text("Materials: %zu", model->materials.size());

            // wireframe toggle
            if (auto* vulkanContext = VulkanContext::getContextInstance()) {
                bool wireframe = vulkanContext->isWireframeMode();
                if (ImGui::Checkbox("Wireframe Mode", &wireframe)) {
                    vulkanContext->setWireframeMode(wireframe);
                }
                // scene-wide properties
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No model assigned");
        }
    }
}

} // namespace ohao
