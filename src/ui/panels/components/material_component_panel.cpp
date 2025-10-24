#include "material_component_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>

namespace ohao {

MaterialComponentPanel::MaterialComponentPanel() : PanelBase("Material Component") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void MaterialComponentPanel::render() {
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
            auto materialComponent = m_selectedActor->getComponent<MaterialComponent>();
            if (materialComponent) {
                renderMaterialProperties(materialComponent.get());
            } else {
                ImGui::TextDisabled("No MaterialComponent found on selected actor");
            }
        } else {
            ImGui::TextDisabled("No actor selected");
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void MaterialComponentPanel::renderMaterialProperties(MaterialComponent* component) {
    if (!component) return;

    ImGui::Text("Material Component Properties");
    ImGui::Separator();

    // Material preset selection
    const char* materialTypeNames[] = {
        "Custom", "Metal", "Plastic", "Glass", "Rubber",
        "Fabric", "Skin", "Wood", "Concrete", "Gold",
        "Silver", "Copper", "Chrome"
    };

    int currentType = static_cast<int>(component->getMaterial().type);
    if (ImGui::Combo("Material Preset##material_preset", &currentType, materialTypeNames, 13)) {
        component->applyPreset(static_cast<Material::Type>(currentType));
    }

    // Material name
    Material& material = component->getMaterial();
    char nameBuffer[256];
    std::strncpy(nameBuffer, material.name.c_str(), sizeof(nameBuffer) - 1);
    nameBuffer[sizeof(nameBuffer) - 1] = '\0';
    if (ImGui::InputText("Material Name##material_name", nameBuffer, sizeof(nameBuffer))) {
        material.name = std::string(nameBuffer);
    }

    ImGui::Spacing();

    // PBR Material Properties
    if (ImGui::CollapsingHeader("PBR Properties##pbr_props", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderPBRMaterialProperties(material);
    }

    // Texture Properties
    if (ImGui::CollapsingHeader("Textures##texture_props", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Texture Maps:");

        // Albedo texture
        if (material.useAlbedoTexture && !material.albedoTexture.empty()) {
            ImGui::Text("Albedo: %s", material.albedoTexture.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove##albedo")) {
                component->setAlbedoTexture("");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No albedo texture");
            ImGui::SameLine();
            if (ImGui::Button("Add##albedo")) {
                // TODO: Open file dialog for texture selection
                ImGui::OpenPopup("AlbedoTextureDialog");
            }
        }

        // Normal texture
        if (material.useNormalTexture && !material.normalTexture.empty()) {
            ImGui::Text("Normal: %s", material.normalTexture.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove##normal")) {
                component->setNormalTexture("");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No normal texture");
            ImGui::SameLine();
            if (ImGui::Button("Add##normal")) {
                // TODO: Open file dialog for texture selection
                ImGui::OpenPopup("NormalTextureDialog");
            }
        }

        // Metallic texture
        if (material.useMetallicTexture && !material.metallicTexture.empty()) {
            ImGui::Text("Metallic: %s", material.metallicTexture.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove##metallic")) {
                component->setMetallicTexture("");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No metallic texture");
            ImGui::SameLine();
            if (ImGui::Button("Add##metallic")) {
                // TODO: Open file dialog for texture selection
                ImGui::OpenPopup("MetallicTextureDialog");
            }
        }

        // Roughness texture
        if (material.useRoughnessTexture && !material.roughnessTexture.empty()) {
            ImGui::Text("Roughness: %s", material.roughnessTexture.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove##roughness")) {
                component->setRoughnessTexture("");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No roughness texture");
            ImGui::SameLine();
            if (ImGui::Button("Add##roughness")) {
                // TODO: Open file dialog for texture selection
                ImGui::OpenPopup("RoughnessTextureDialog");
            }
        }

        // AO texture
        if (material.useAoTexture && !material.aoTexture.empty()) {
            ImGui::Text("AO: %s", material.aoTexture.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove##ao")) {
                component->setAoTexture("");
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No AO texture");
            ImGui::SameLine();
            if (ImGui::Button("Add##ao")) {
                // TODO: Open file dialog for texture selection
                ImGui::OpenPopup("AoTextureDialog");
            }
        }

        // Texture file dialogs (unique for each texture type)
        if (ImGui::BeginPopup("AlbedoTextureDialog")) {
            ImGui::Text("Albedo texture file selection not yet implemented");
            if (ImGui::Button("Close##albedo_close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("NormalTextureDialog")) {
            ImGui::Text("Normal texture file selection not yet implemented");
            if (ImGui::Button("Close##normal_close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("MetallicTextureDialog")) {
            ImGui::Text("Metallic texture file selection not yet implemented");
            if (ImGui::Button("Close##metallic_close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("RoughnessTextureDialog")) {
            ImGui::Text("Roughness texture file selection not yet implemented");
            if (ImGui::Button("Close##roughness_close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("AoTextureDialog")) {
            ImGui::Text("AO texture file selection not yet implemented");
            if (ImGui::Button("Close##ao_close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void MaterialComponentPanel::renderPBRMaterialProperties(Material& material) {
    ImGui::Text("PBR Material Properties");
    ImGui::Separator();

    // Core PBR Properties
    ImGui::Text("Core PBR Properties");

    // Base Color
    if (ImGui::ColorEdit3("Base Color", glm::value_ptr(material.baseColor))) {
        // Color changed
    }

    // Metallic
    if (ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f)) {
        // Metallic changed
    }

    // Roughness
    if (ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f)) {
        // Roughness changed
    }

    // Ambient Occlusion
    if (ImGui::SliderFloat("Ambient Occlusion", &material.ao, 0.0f, 1.0f)) {
        // AO changed
    }

    // Advanced Properties
    if (ImGui::CollapsingHeader("Advanced Properties")) {
        // Emissive
        if (ImGui::ColorEdit3("Emissive", glm::value_ptr(material.emissive))) {
            // Emissive changed
        }

        // Index of Refraction
        if (ImGui::SliderFloat("IOR", &material.ior, 1.0f, 2.5f)) {
            // IOR changed
        }

        // Transmission (for glass)
        if (ImGui::SliderFloat("Transmission", &material.transmission, 0.0f, 1.0f)) {
            // Transmission changed
        }

        // Clear Coat
        if (ImGui::SliderFloat("Clear Coat", &material.clearCoat, 0.0f, 1.0f)) {
            // Clear coat changed
        }

        if (material.clearCoat > 0.0f) {
            if (ImGui::SliderFloat("Clear Coat Roughness", &material.clearCoatRoughness, 0.0f, 1.0f)) {
                // Clear coat roughness changed
            }
        }

        // Subsurface Scattering
        if (ImGui::ColorEdit3("Subsurface Scattering", glm::value_ptr(material.subsurface))) {
            // Subsurface changed
        }

        if (glm::length(material.subsurface) > 0.0f) {
            if (ImGui::SliderFloat("Subsurface Radius", &material.subsurfaceRadius, 0.1f, 10.0f)) {
                // Subsurface radius changed
            }
        }

        // Normal and Height mapping
        if (ImGui::SliderFloat("Normal Intensity", &material.normalIntensity, 0.0f, 2.0f)) {
            // Normal intensity changed
        }

        if (ImGui::SliderFloat("Height Scale", &material.heightScale, 0.0f, 0.2f)) {
            // Height scale changed
        }
    }

    // Material Information
    if (ImGui::CollapsingHeader("Material Info")) {
        ImGui::Text("Name: %s", material.name.c_str());

        // Display material type name
        const char* typeNames[] = {
            "Custom", "Metal", "Plastic", "Glass", "Rubber", "Fabric",
            "Skin", "Wood", "Concrete", "Gold", "Silver", "Copper", "Chrome"
        };
        int typeIndex = static_cast<int>(material.type);
        if (typeIndex >= 0 && typeIndex < 13) {
            ImGui::Text("Type: %s", typeNames[typeIndex]);
        } else {
            ImGui::Text("Type: Unknown");
        }

        // Display computed F0 value for reference
        glm::vec3 F0 = glm::mix(glm::vec3(0.04f), material.baseColor, material.metallic);
        ImGui::Text("F0: (%.3f, %.3f, %.3f)", F0.x, F0.y, F0.z);

        // Display whether material is metal or dielectric
        ImGui::Text("Classification: %s", material.metallic > 0.5f ? "Metallic" : "Dielectric");
    }

    // Quick Preset Buttons
    if (ImGui::CollapsingHeader("Quick Presets")) {
        ImGui::Columns(3, nullptr, false);

        if (ImGui::Button("Gold", ImVec2(-1, 0))) {
            material = Material::createGold();
        }
        ImGui::NextColumn();

        if (ImGui::Button("Silver", ImVec2(-1, 0))) {
            material = Material::createSilver();
        }
        ImGui::NextColumn();

        if (ImGui::Button("Chrome", ImVec2(-1, 0))) {
            material = Material::createChrome();
        }
        ImGui::NextColumn();

        if (ImGui::Button("Plastic", ImVec2(-1, 0))) {
            material = Material::createPlastic(glm::vec3(0.8f, 0.2f, 0.2f));
        }
        ImGui::NextColumn();

        if (ImGui::Button("Glass", ImVec2(-1, 0))) {
            material = Material::createGlass();
        }
        ImGui::NextColumn();

        if (ImGui::Button("Rubber", ImVec2(-1, 0))) {
            material = Material::createRubber(glm::vec3(0.2f, 0.2f, 0.2f));
        }

        ImGui::Columns(1);
    }
}

} // namespace ohao
