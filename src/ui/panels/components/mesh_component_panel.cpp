#include "mesh_component_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "vulkan_context.hpp"
#include <glm/glm.hpp>

namespace ohao {

MeshComponentPanel::MeshComponentPanel() : PanelBase("Mesh Component") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void MeshComponentPanel::render() {
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
            auto meshComponent = m_selectedActor->getComponent<MeshComponent>();
            if (meshComponent) {
                renderMeshProperties(meshComponent.get());
            } else {
                ImGui::TextDisabled("No MeshComponent found on selected actor");
            }
        } else {
            ImGui::TextDisabled("No actor selected");
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void MeshComponentPanel::renderMeshProperties(MeshComponent* component) {
    if (!component) return;

    ImGui::Text("Mesh Component Properties");
    ImGui::Separator();

    // Show model information if available
    auto model = component->getModel();
    if (model) {
        ImGui::Text("Model Information:");
        ImGui::Indent();
        ImGui::Text("Vertices: %zu", model->vertices.size());
        ImGui::Text("Indices: %zu", model->indices.size());
        ImGui::Text("Materials: %zu", model->materials.size());
        ImGui::Unindent();

        ImGui::Spacing();
        // Option to replace the model
        if (ImGui::Button("Replace Model", ImVec2(150, 0))) {
            ImGui::OpenPopup("ReplaceModelPopup");
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No model assigned");

        ImGui::Spacing();
        // Button to add a model
        if (ImGui::Button("Add Model", ImVec2(150, 0))) {
            ImGui::OpenPopup("AddModelPopup");
        }
    }

    // Model selection popup
    if (ImGui::BeginPopup("AddModelPopup") || ImGui::BeginPopup("ReplaceModelPopup")) {
        ImGui::Text("Select Primitive Type:");
        ImGui::Separator();

        if (ImGui::Selectable("Cube")) {
            auto cubeModel = generatePrimitiveMesh(PrimitiveType::Cube);
            component->setModel(cubeModel);
            ImGui::CloseCurrentPopup();

            // Update scene buffers
            if (auto context = VulkanContext::getContextInstance()) {
                context->updateSceneBuffers();
            }
        }

        if (ImGui::Selectable("Sphere")) {
            auto sphereModel = generatePrimitiveMesh(PrimitiveType::Sphere);
            component->setModel(sphereModel);
            ImGui::CloseCurrentPopup();

            // Update scene buffers
            if (auto context = VulkanContext::getContextInstance()) {
                context->updateSceneBuffers();
            }
        }

        if (ImGui::Selectable("Platform")) {
            auto platformModel = generatePrimitiveMesh(PrimitiveType::Platform);
            component->setModel(platformModel);
            ImGui::CloseCurrentPopup();

            // Update scene buffers
            if (auto context = VulkanContext::getContextInstance()) {
                context->updateSceneBuffers();
            }
        }

        ImGui::Separator();

        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

std::shared_ptr<Model> MeshComponentPanel::generatePrimitiveMesh(PrimitiveType type) {
    auto model = std::make_shared<Model>();

    switch (type) {
        case PrimitiveType::Cube:
        {
            const float size = 1.0f;
            const float hs = size * 0.5f; // half size

            // Vertices for a cube with proper normals and UVs
            std::vector<Vertex> vertices = {
                // Front face
                {{-hs, -hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {0, 0}}, // 0
                {{ hs, -hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {1, 0}}, // 1
                {{ hs,  hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {1, 1}}, // 2
                {{-hs,  hs,  hs}, {1, 1, 1}, { 0,  0,  1}, {0, 1}}, // 3

                // Back face
                {{ hs, -hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {0, 0}}, // 4
                {{-hs, -hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {1, 0}}, // 5
                {{-hs,  hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {1, 1}}, // 6
                {{ hs,  hs, -hs}, {1, 1, 1}, { 0,  0, -1}, {0, 1}}, // 7

                // Top face
                {{-hs,  hs, -hs}, {1, 1, 1}, { 0,  1,  0}, {0, 0}}, // 8
                {{ hs,  hs, -hs}, {1, 1, 1}, { 0,  1,  0}, {1, 0}}, // 9
                {{ hs,  hs,  hs}, {1, 1, 1}, { 0,  1,  0}, {1, 1}}, // 10
                {{-hs,  hs,  hs}, {1, 1, 1}, { 0,  1,  0}, {0, 1}}, // 11

                // Bottom face
                {{-hs, -hs, -hs}, {1, 1, 1}, { 0, -1,  0}, {0, 0}}, // 12
                {{ hs, -hs, -hs}, {1, 1, 1}, { 0, -1,  0}, {1, 0}}, // 13
                {{ hs, -hs,  hs}, {1, 1, 1}, { 0, -1,  0}, {1, 1}}, // 14
                {{-hs, -hs,  hs}, {1, 1, 1}, { 0, -1,  0}, {0, 1}}, // 15

                // Right face
                {{ hs, -hs,  hs}, {1, 1, 1}, { 1,  0,  0}, {0, 0}}, // 16
                {{ hs, -hs, -hs}, {1, 1, 1}, { 1,  0,  0}, {1, 0}}, // 17
                {{ hs,  hs, -hs}, {1, 1, 1}, { 1,  0,  0}, {1, 1}}, // 18
                {{ hs,  hs,  hs}, {1, 1, 1}, { 1,  0,  0}, {0, 1}}, // 19

                // Left face
                {{-hs, -hs, -hs}, {1, 1, 1}, {-1,  0,  0}, {0, 0}}, // 20
                {{-hs, -hs,  hs}, {1, 1, 1}, {-1,  0,  0}, {1, 0}}, // 21
                {{-hs,  hs,  hs}, {1, 1, 1}, {-1,  0,  0}, {1, 1}}, // 22
                {{-hs,  hs, -hs}, {1, 1, 1}, {-1,  0,  0}, {0, 1}}  // 23
            };

            // Indices for the cube
            std::vector<uint32_t> indices = {
                0,  1,  2,  2,  3,  0,  // Front
                4,  5,  6,  6,  7,  4,  // Back
                8,  9,  10, 10, 11, 8,  // Top
                12, 13, 14, 14, 15, 12, // Bottom
                16, 17, 18, 18, 19, 16, // Right
                20, 21, 22, 22, 23, 20  // Left
            };

            model->vertices = vertices;
            model->indices = indices;
            break;
        }

        case PrimitiveType::Sphere:
        {
            const float radius = 0.5f;
            const int sectors = 32;  // longitude
            const int stacks = 16;   // latitude

            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;

            // Generate vertices
            for (int i = 0; i <= stacks; ++i) {
                float phi = glm::pi<float>() * float(i) / float(stacks);
                float sinPhi = sin(phi);
                float cosPhi = cos(phi);

                for (int j = 0; j <= sectors; ++j) {
                    float theta = 2.0f * glm::pi<float>() * float(j) / float(sectors);
                    float sinTheta = sin(theta);
                    float cosTheta = cos(theta);

                    float x = cosTheta * sinPhi;
                    float y = cosPhi;
                    float z = sinTheta * sinPhi;

                    Vertex vertex;
                    vertex.position = {x * radius, y * radius, z * radius};
                    vertex.normal = {x, y, z};  // Normalized position = normal for sphere
                    vertex.color = {1.0f, 1.0f, 1.0f};
                    vertex.texCoord = {float(j) / sectors, float(i) / stacks};

                    vertices.push_back(vertex);
                }
            }

            // Generate indices
            for (int i = 0; i < stacks; ++i) {
                for (int j = 0; j < sectors; ++j) {
                    int first = i * (sectors + 1) + j;
                    int second = first + sectors + 1;

                    indices.push_back(first);
                    indices.push_back(second);
                    indices.push_back(first + 1);

                    indices.push_back(second);
                    indices.push_back(second + 1);
                    indices.push_back(first + 1);
                }
            }

            model->vertices = vertices;
            model->indices = indices;
            break;
        }

        case PrimitiveType::Platform:
        {
            const float size = 1.0f;
            const int subdivisions = 1;  // Increase for more detailed plane
            const float step = size / subdivisions;
            const float uvStep = 1.0f / subdivisions;

            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;

            // Generate vertices
            for (int i = 0; i <= subdivisions; ++i) {
                for (int j = 0; j <= subdivisions; ++j) {
                    float x = -size/2 + j * step;
                    float z = -size/2 + i * step;

                    Vertex vertex;
                    vertex.position = {x, 0.0f, z};
                    vertex.normal = {0.0f, 1.0f, 0.0f};
                    vertex.color = {1.0f, 1.0f, 1.0f};
                    vertex.texCoord = {j * uvStep, i * uvStep};

                    vertices.push_back(vertex);
                }
            }

            // Generate indices
            for (int i = 0; i < subdivisions; ++i) {
                for (int j = 0; j < subdivisions; ++j) {
                    int row1 = i * (subdivisions + 1);
                    int row2 = (i + 1) * (subdivisions + 1);

                    // Triangle 1
                    indices.push_back(row1 + j);
                    indices.push_back(row1 + j + 1);
                    indices.push_back(row2 + j + 1);

                    // Triangle 2
                    indices.push_back(row1 + j);
                    indices.push_back(row2 + j + 1);
                    indices.push_back(row2 + j);
                }
            }

            model->vertices = vertices;
            model->indices = indices;
            break;
        }

        case PrimitiveType::Empty:
        default:
            // Empty object has no geometry
            break;
    }

    // Setup default material
    MaterialData defaultMaterial;
    defaultMaterial.name = "Default";
    defaultMaterial.ambient = glm::vec3(0.2f);
    defaultMaterial.diffuse = glm::vec3(0.8f);
    defaultMaterial.specular = glm::vec3(0.5f);
    defaultMaterial.shininess = 32.0f;
    model->materials["default"] = defaultMaterial;

    return model;
}

} // namespace ohao
