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

    renderViewModeSelector();

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (selectedNode) {
            handleObjectDeletion(selectedNode);
        }
    }

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
    if (currentViewMode == ViewMode::List) {
        if (currentScene && currentScene->getRootNode()) {
            renderTreeNode(currentScene->getRootNode().get());
        }
    } else {
        renderGraphView();
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

    // Allow selection for all nodes
    if (node == selectedNode) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    bool isLeaf = node->getChildren().empty();
    if (isLeaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // Format node label (Blender style)
    std::string nodeLabel;
    ImVec4 textColor;

    if (isRoot(node)) {
        nodeLabel = "Scene Collection";
        textColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    } else if (isSceneObject(node)) {
        auto sceneObj = asSceneObject(node);
        if (sceneObj->getModel()) {
            nodeLabel = "\uf1b2 " + node->getName();  // Cube icon for mesh objects
        } else {
            nodeLabel = "\uf192 " + node->getName();  // Dot icon for empty objects
        }
        textColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    } else {
        nodeLabel = node->getName();
        textColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    bool opened = ImGui::TreeNodeEx(nodeLabel.c_str(), flags);
    ImGui::PopStyleColor();

    // Handle selection
    if (ImGui::IsItemClicked()) {
        selectedNode = node;
        if (!isRoot(node) && isSceneObject(node)) {
            SelectionManager::get().setSelectedObject(asSceneObject(node));
        } else {
            // Allow root selection for parenting but clear viewport selection
            SelectionManager::get().clearSelection();
        }
    }

    // Context menu
    if (ImGui::IsItemClicked(1)) {
        contextMenuTarget = node;
        showContextMenu = true;
    }

    // Drag and drop
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("SCENE_NODE", &node, sizeof(SceneNode*));
        ImGui::Text("Moving %s", node->getName().c_str());
        ImGui::EndDragDropSource();
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
    if (ImGui::MenuItem("Add Child")) {
        // Create child under selected node
        createPrimitiveObject(PrimitiveType::Empty);
    }

    if (!isRoot(node)) {  // Don't allow these operations on root
        if (ImGui::MenuItem("Rename")) {
            // Start rename operation
        }
        if (ImGui::MenuItem("Duplicate")) {
            if (auto sceneObj = asSceneObject(node)) {
                auto duplicate = sceneObj->clone();
                if (auto parent = node->getParent()) {
                    parent->addChild(duplicate);
                    currentScene->addObject(duplicate->getName(), duplicate);
                }
            }
        }
        if (ImGui::MenuItem("Delete")) {
            handleObjectDeletion(node);
        }
    }
}

void OutlinerPanel::handleObjectDeletion(SceneNode* node) {
    if (!node || !currentScene) return;

    OHAO_LOG_DEBUG("Starting deletion of node: " + node->getName());

    // Clear selection first if this object is selected
    if (auto sceneObj = asSceneObject(node)) {
        if (SelectionManager::get().isSelected(sceneObj)) {
            SelectionManager::get().clearSelection();
        }
    }

    try {
        // Remove from scene's object collection first
        if (auto sceneObj = asSceneObject(node)) {
            currentScene->removeObject(sceneObj->getName());
        }

        // Remove from scene hierarchy
        if (node->getParent()) {
            node->detachFromParent();
        }

        // Clear selection if this was the selected node
        if (selectedNode == node) {
            selectedNode = nullptr;
        }

        // Update the buffers
        if (auto context = VulkanContext::getContextInstance()) {
            context->updateSceneBuffers();
        }

        OHAO_LOG_DEBUG("Successfully deleted node");

    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error during node deletion: " + std::string(e.what()));
    }
}

void OutlinerPanel::createPrimitiveObject(PrimitiveType type) {
    if (!currentScene) {
        OHAO_LOG_ERROR("No active scene!");
        return;
    }

    // Get parent node - use selected node if available, otherwise use root
    SceneNode* parentNode = selectedNode ? selectedNode : currentScene->getRootNode().get();

    try {
        std::shared_ptr<SceneObject> newObject;
        std::string baseName;

        switch (type) {
            case PrimitiveType::Empty:
                baseName = "Empty";
                newObject = std::make_shared<SceneObject>(baseName);
                break;

            case PrimitiveType::Cube:
            case PrimitiveType::Sphere:
            case PrimitiveType::Plane:
                baseName = (type == PrimitiveType::Cube) ? "Cube" :
                          (type == PrimitiveType::Sphere) ? "Sphere" : "Plane";
                newObject = std::make_shared<SceneObject>(baseName);
                auto model = generatePrimitiveMesh(type);
                newObject->setModel(model);

                // Set initial transform
                Transform transform;
                // If parent is not root, offset slightly from parent's position
                if (!isRoot(parentNode)) {
                    auto parentPos = parentNode->getTransform().getWorldPosition();
                    transform.setLocalPosition(parentPos + glm::vec3(0.5f, 0.5f, 0.0f));
                } else {
                    transform.setLocalPosition(glm::vec3(0.0f));
                }
                newObject->setTransform(transform);
                break;
        }

        if (newObject) {
            // Generate unique name (Blender style)
            std::string uniqueName = baseName;
            int counter = 1;
            while (currentScene->getObjects().find(uniqueName) != currentScene->getObjects().end()) {
                uniqueName = baseName + "." + std::to_string(counter++).substr(0, 3);
            }
            newObject->setName(uniqueName);
            currentScene->addObject(uniqueName, newObject);
            parentNode->addChild(newObject);
            OHAO_LOG_DEBUG("Created new " + uniqueName + " under parent: " + parentNode->getName());

            // Update buffers
            if (auto* vulkanContext = VulkanContext::getContextInstance()) {
                if (!vulkanContext->updateSceneBuffers()) {
                    OHAO_LOG_ERROR("Failed to update scene buffers");
                    return;
                }
            }

            // Select the newly created object
            selectedNode = newObject.get();
            SelectionManager::get().setSelectedObject(newObject.get());

            OHAO_LOG("Created new " + uniqueName + " under " + parentNode->getName());
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

void OutlinerPanel::renderViewModeSelector() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    if (ImGui::Button(currentViewMode == ViewMode::List ? "List View" : "Graph View")) {
        currentViewMode = (currentViewMode == ViewMode::List) ? ViewMode::Graph : ViewMode::List;
    }
    ImGui::PopStyleVar();

    if (currentViewMode == ViewMode::Graph) {
        ImGui::SameLine();
        ImGui::DragFloat("Zoom", &graphZoom, 0.01f, 0.1f, 2.0f);
    }
}

void OutlinerPanel::renderGraphView() {
    if (!currentScene || !currentScene->getRootNode()) return;

    // Create a child window for the graph
    ImGui::BeginChild("GraphView", ImVec2(0, 0), true,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

    handleGraphNavigation();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    // Start position for root node
    ImVec2 startPos = canvasPos;
    startPos.x += graphOffset.x;
    startPos.y += graphOffset.y;

    // Render nodes
    renderGraphNode(currentScene->getRootNode().get(), startPos, 0);

    ImGui::EndChild();
}

void OutlinerPanel::renderGraphNode(SceneNode* node, ImVec2& pos, int depth) {
    if (!node) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 nodePos = pos;

    // Calculate node size based on zoom
    float scaledNodeSize = graphNodeSize * graphZoom;
    ImVec2 nodeSize(scaledNodeSize, scaledNodeSize);

    bool nodeIsRoot = isRoot(node);
    bool isSelected = (node == selectedNode && !nodeIsRoot);

    // Different colors for root, selected, and regular nodes
    ImU32 nodeColor;
    if (nodeIsRoot) {
        nodeColor = IM_COL32(100, 100, 100, 200);
    } else if (isSelected) {
        nodeColor = IM_COL32(255, 165, 0, 255);
    } else {
        nodeColor = IM_COL32(100, 100, 100, 255);
    }

    float rounding = scaledNodeSize * 0.2f;

    // Draw node with different style for root
    if (nodeIsRoot) {
        drawList->AddRect(
            nodePos,
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
            nodeColor,
            rounding,
            ImDrawFlags_None,
            2.0f
        );
    } else {
        drawList->AddRectFilled(
            nodePos,
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
            nodeColor,
            rounding
        );
    }

    // Draw node label with different color for root
    ImVec2 textPos = nodePos;
    textPos.x += nodeSize.x * 0.5f;
    textPos.y += nodeSize.y + 5.0f;
    ImU32 textColor = nodeIsRoot ?
        IM_COL32(150, 150, 150, 255) :
        IM_COL32(255, 255, 255, 255);

    std::string label = nodeIsRoot ? "Root" : node->getName();
    drawList->AddText(
        textPos,
        textColor,
        label.c_str()
    );

    // Handle node selection (disable for root)
    if (!nodeIsRoot) {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (ImGui::IsMouseClicked(0)) {
            if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y) {
                selectedNode = node;
                if (!isRoot(node) && isSceneObject(node)) {
                    SelectionManager::get().setSelectedObject(asSceneObject(node));
                } else {
                    SelectionManager::get().clearSelection();
                }
            }
        }
    }

    // Calculate and draw connections to children
    float childSpacing = graphSpacingX * graphZoom;
    float verticalSpacing = graphSpacingY * graphZoom;

    const auto& children = node->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
        ImVec2 childPos = nodePos;
        childPos.x += childSpacing;
        childPos.y += verticalSpacing * (i - (children.size() - 1) * 0.5f);

        // Draw connection line
        drawList->AddLine(
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y * 0.5f),
            ImVec2(childPos.x, childPos.y + nodeSize.y * 0.5f),
            IM_COL32(150, 150, 150, 255),
            2.0f
        );

        // Render child node
        renderGraphNode(children[i].get(), childPos, depth + 1);
    }
}

void OutlinerPanel::handleGraphNavigation() {
    // Pan with middle mouse button
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        graphOffset.x += ImGui::GetIO().MouseDelta.x;
        graphOffset.y += ImGui::GetIO().MouseDelta.y;
    }

    // Zoom with mouse wheel
    if (ImGui::GetIO().MouseWheel != 0 && ImGui::IsWindowHovered()) {
        float zoomDelta = ImGui::GetIO().MouseWheel * 0.1f;
        graphZoom = std::clamp(graphZoom + zoomDelta, 0.1f, 2.0f);
    }
}

bool OutlinerPanel::isRoot(SceneNode* node) const {
    return currentScene && node == currentScene->getRootNode().get();
}

bool OutlinerPanel::isSceneObject(SceneNode* node) const {
    return asSceneObject(node) != nullptr;
}

bool OutlinerPanel::isSelected(SceneNode* node) const {
    return node == selectedNode;
}

} // namespace ohao
