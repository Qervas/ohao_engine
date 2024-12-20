#include "outliner_panel.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#include "scene/scene_node.hpp"
#include "vulkan_context.hpp"

namespace ohao {

OutlinerPanel::OutlinerPanel() : PanelBase("Outliner") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void OutlinerPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    // Toolbar
    if (ImGui::Button("Add")) {
        ImGui::OpenPopup("AddObjectPopup");
    }

    if (ImGui::BeginPopup("AddObjectPopup")) {
        if (ImGui::MenuItem("Empty")) {
            createPrimitiveObject(PrimitiveType::Empty);
        }
        if (ImGui::MenuItem("Cube")) {
            createPrimitiveObject(PrimitiveType::Cube);
        }
        if (ImGui::MenuItem("Sphere")) {
            createPrimitiveObject(PrimitiveType::Sphere);
        }
        if (ImGui::MenuItem("Plane")) {
            createPrimitiveObject(PrimitiveType::Plane);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && selectedNode) {
        handleObjectDeletion(selectedNode);
    }

    ImGui::Separator();

    // Scene Tree
    if (currentScene && currentScene->getRootNode()) {
        renderTreeNode(currentScene->getRootNode().get());
    }

    handleDragAndDrop();

    if (showContextMenu) {
        ImGui::OpenPopup("ObjectContextMenu");
        showContextMenu = false;
    }

    if (ImGui::BeginPopup("ObjectContextMenu") && contextMenuTarget) {
        showObjectContextMenu(contextMenuTarget);
        ImGui::EndPopup();
    }

    ImGui::End();
}

void OutlinerPanel::renderSceneTree() {
    for (const auto& [name, object] : currentScene->getObjects()) {
        renderTreeNode(object.get());
    }
}

void OutlinerPanel::renderTreeNode(SceneNode* node) {
    if (!node) return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                              ImGuiTreeNodeFlags_OpenOnDoubleClick |
                              ImGuiTreeNodeFlags_SpanAvailWidth;

    if (node == selectedNode) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool isLeaf = node->getChildren().empty();
    if (isLeaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // Show different icon/text for SceneObjects vs regular nodes
    std::string nodeLabel = node->getName();
    if (auto sceneObj = asSceneObject(node)) {
        nodeLabel = "[Object] " + nodeLabel;
    }

    bool opened = ImGui::TreeNodeEx(nodeLabel.c_str(), flags);

    if (ImGui::IsItemClicked()) {
        selectedNode = node;
        if (auto sceneObj = asSceneObject(node)) {
            SelectionManager::get().setSelectedObject(sceneObj);
        }
    }

    if (ImGui::IsItemClicked(1)) {
        contextMenuTarget = node;
        showContextMenu = true;
    }

    if (opened) {
        for (const auto& child : node->getChildren()) {
            renderTreeNode(child.get());
        }
        ImGui::TreePop();
    }
}

void OutlinerPanel::handleDragAndDrop() {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_NODE")) {
            SceneNode* droppedNode = *(SceneNode**)payload->Data;
            // Handle node reparenting
            if (droppedNode && selectedNode) {
                droppedNode->detachFromParent();
                selectedNode->addChild(std::shared_ptr<SceneNode>(droppedNode));
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void OutlinerPanel::showObjectContextMenu(SceneNode* node) {
    if (ImGui::MenuItem("Rename")) {
         // Start rename operation
     }
     if (ImGui::MenuItem("Duplicate")) {
         // Duplicate node
         if (auto sceneObj = asSceneObject(node)) {
             auto duplicate = sceneObj->clone();
             duplicate->setName(duplicate->getName() + "_copy");
             if (auto parent = node->getParent()) {
                 parent->addChild(duplicate);
             }
         }
     }
     if (ImGui::MenuItem("Delete")) {
         handleObjectDeletion(node);
     }

     ImGui::Separator();

     if (ImGui::MenuItem("Add Child")) {
         auto newChild = std::make_shared<SceneObject>("New Child");
         node->addChild(newChild);
     }
}

void OutlinerPanel::handleObjectDeletion(SceneNode* node) {
    if (!node || !currentScene) return;

    // Remove from parent
    node->detachFromParent();

    if (selectedNode == node) {
        selectedNode = nullptr;
        if (auto sceneObj = asSceneObject(node)) {
            SelectionManager::get().clearSelection();
        }
    }
}

void OutlinerPanel::createPrimitiveObject(PrimitiveType type) {
    if (!currentScene) {
        OHAO_LOG_ERROR("No active scene!");
        return;
    }

    try {
        std::shared_ptr<SceneObject> newObject;
        std::string name;

        switch (type) {
            case PrimitiveType::Empty:
                name = "Empty";
                newObject = std::make_shared<SceneObject>(name);
                break;

            case PrimitiveType::Cube:
            case PrimitiveType::Sphere:
            case PrimitiveType::Plane:
                {
                    name = (type == PrimitiveType::Cube) ? "Cube" :
                          (type == PrimitiveType::Sphere) ? "Sphere" : "Plane";

                    newObject = std::make_shared<SceneObject>(name);
                    auto model = generatePrimitiveMesh(type);
                    newObject->setModel(model);

                    // Update buffers in VulkanContext
                    if (auto* vulkanContext = VulkanContext::getContextInstance()) {
                        vulkanContext->getLogicalDevice()->waitIdle();  // Wait before update
                        if (!vulkanContext->updateModelBuffers(model->vertices, model->indices)) {
                            OHAO_LOG_ERROR("Failed to create buffers for " + name);
                            return;
                        }
                        vulkanContext->getLogicalDevice()->waitIdle();  // Wait after update
                    }
                }
                break;
        }

        if (newObject) {
            currentScene->getRootNode()->addChild(newObject);
            SelectionManager::get().setSelectedObject(newObject.get());
            OHAO_LOG("Created new " + name);
        }

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Failed to create primitive: " + std::string(e.what()));
    }
}

std::shared_ptr<Model> OutlinerPanel::generatePrimitiveMesh(PrimitiveType type) {
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
            OHAO_LOG("Cube added");
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
            OHAO_LOG("Sphere added");
            break;
        }

        case PrimitiveType::Plane:
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
            OHAO_LOG("Plane added");
            break;
        }

        case PrimitiveType::Empty:
            // Empty object has no geometry
            OHAO_LOG("Empty object added");
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
