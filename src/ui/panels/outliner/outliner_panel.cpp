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
        renderSceneTree();
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
    if (!currentScene) return;
    
    // First render the root node and its hierarchy
    if (auto rootNode = currentScene->getRootNode()) {
        // Use PushID to create a unique context for the root node
        ImGui::PushID("root");
        renderTreeNode(rootNode.get());
        ImGui::PopID();
    }
    
    // Then render any actors that might not be in the hierarchy
    // This ensures all objects are visible in the outliner
    bool hasOrphanedActors = false;
    
    // Get all actors from the scene
    const auto& allActors = currentScene->getAllActors();
    if (!allActors.empty()) {
        for (const auto& [id, actor] : allActors) {
            // Only render actors that don't have a parent (top-level actors)
            // AND are not the root node itself
            if (actor && !actor->getParent() && 
                (!currentScene->getRootNode() || actor.get() != currentScene->getRootNode().get())) {
                if (!hasOrphanedActors) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "Scene Objects");
                    hasOrphanedActors = true;
                }
                
                ImGui::PushID(static_cast<int>(id));
                renderTreeNode(actor.get());
                ImGui::PopID();
            }
        }
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

    // Check if this is an Actor
    Actor* actorNode = dynamic_cast<Actor*>(node);
    bool isLeaf = actorNode ? actorNode->getChildren().empty() : node->getChildren().empty();
    
    if (isLeaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // Format node label (Blender style)
    std::string nodeLabel;
    ImVec4 textColor;

    if (isRoot(node)) {
        // Add a unique identifier to the root node to avoid ID conflicts
        nodeLabel = "Scene Collection##SceneRoot";
        textColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    } else if (auto actor = dynamic_cast<Actor*>(node)) {
        // Use actor ID as a unique identifier
        std::string idStr = "##" + std::to_string(actor->getID());
        if (actor->getModel()) {
            nodeLabel = "\uf1b2 " + node->getName() + idStr;
        } else {
            nodeLabel = "\uf192 " + node->getName() + idStr;
        }
        textColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    } else if (isSceneObject(node)) {
        auto sceneObj = asSceneObject(node);
        // Use object ID as a unique identifier
        std::string idStr = "##" + std::to_string(sceneObj->getID());
        if (sceneObj->getModel()) {
            nodeLabel = "\uf1b2 " + node->getName() + idStr;
        } else {
            nodeLabel = "\uf192 " + node->getName() + idStr;
        }
        textColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    } else {
        // For other nodes, use pointer address as unique ID
        nodeLabel = node->getName() + "##" + std::to_string(reinterpret_cast<uintptr_t>(node));
        textColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    bool opened = ImGui::TreeNodeEx(nodeLabel.c_str(), flags);
    ImGui::PopStyleColor();

    // Handle selection
    if (ImGui::IsItemClicked()) {
        selectedNode = node;
        if (auto actor = dynamic_cast<Actor*>(node)) {
            // Use the new Actor selection system
            SelectionManager::get().setSelectedActor(actor);
        } else if (isSceneObject(node)) {
            auto sceneObj = asSceneObject(node);
            // Use backward compatibility method
            SelectionManager::get().setSelectedObject(sceneObj);
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
        if (isSceneObject(node)) {
            auto obj = asSceneObject(node);
            ObjectID objID = obj->getID();
            ImGui::SetDragDropPayload("SCENE_OBJECT_ID", &objID, sizeof(ObjectID));
            ImGui::Text("Moving %s", node->getName().c_str());
        } else {
            // For non-scene objects use pointer
            ImGui::SetDragDropPayload("SCENE_NODE", &node, sizeof(SceneNode*));
            ImGui::Text("Moving %s", node->getName().c_str());
        }
        ImGui::EndDragDropSource();
    }

    if (opened) {
        if (actorNode) {
            for (auto* child : actorNode->getChildren()) {
                renderTreeNode(child);
            }
        } else {
            for (const auto& child : node->getChildren()) {
                renderTreeNode(child.get());
            }
        }
        ImGui::TreePop();
    }
}

void OutlinerPanel::handleDragAndDrop() {
    if (ImGui::BeginDragDropTarget()) {
        // Handle object ID payloads (preferred)
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT_ID")) {
            if (payload->DataSize == sizeof(ObjectID)) {
                ObjectID droppedObjID = *(ObjectID*)payload->Data;
                
                // Find the object in the scene
                if (currentScene) {
                    // Try to find it as an Actor first
                    auto actor = currentScene->findActor(droppedObjID);
                    if (actor && selectedNode) {
                        // Check if the selected node is an Actor
                        if (auto targetActor = dynamic_cast<Actor*>(selectedNode)) {
                            // Actor-to-Actor parenting
                            actor->setParent(targetActor);
                            OHAO_LOG_DEBUG("Reparented actor ID: " + std::to_string(droppedObjID) + 
                                        " to parent: " + targetActor->getName());
                        } else {
                            // Legacy fallback - may not work correctly
                            actor->detachFromParent();
                            selectedNode->addChild(actor);
                            OHAO_LOG_DEBUG("Added actor to scene node: " + std::to_string(droppedObjID));
                        }
                    }
                    
                    // Legacy support for SceneObjects
                    else {
                        auto obj = currentScene->getObjectByID(droppedObjID);
                        if (obj && selectedNode) {
                            obj->detachFromParent();
                            selectedNode->addChild(obj);
                            OHAO_LOG_DEBUG("Reparented object ID: " + std::to_string(droppedObjID) + 
                                        " to parent: " + selectedNode->getName());
                        }
                    }
                }
            }
        }
        // Fallback for non-scene-object nodes
        else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_NODE")) {
            SceneNode* droppedNode = *(SceneNode**)payload->Data;
            // Handle node reparenting
            if (droppedNode && selectedNode) {
                // Check if this is an Actor
                if (auto actorNode = dynamic_cast<Actor*>(droppedNode)) {
                    if (auto targetActor = dynamic_cast<Actor*>(selectedNode)) {
                        // Actor-to-Actor parenting
                        actorNode->setParent(targetActor);
                    } else {
                        // Legacy fallback
                        droppedNode->detachFromParent();
                        selectedNode->addChild(std::shared_ptr<SceneNode>(droppedNode));
                    }
                } else {
                    // Legacy node handling
                    droppedNode->detachFromParent();
                    selectedNode->addChild(std::shared_ptr<SceneNode>(droppedNode));
                }
                OHAO_LOG_DEBUG("Reparented node: " + droppedNode->getName() + 
                            " to parent: " + selectedNode->getName());
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void OutlinerPanel::showObjectContextMenu(SceneNode* node) {
    // Check if node is an Actor
    Actor* actorNode = dynamic_cast<Actor*>(node);
    
    // Actor-specific context menu
    if (actorNode) {
        if (ImGui::MenuItem("Add Child Actor")) {
            // Create a new empty actor and set parent directly
            auto childActor = std::make_shared<Actor>("Child");
            childActor->setParent(actorNode);
            currentScene->addActor(childActor);
        }
        
        if (ImGui::BeginMenu("Add Component")) {
            if (ImGui::MenuItem("Mesh Component")) {
                actorNode->addComponent<MeshComponent>();
            }
            if (ImGui::MenuItem("Physics Component")) {
                try {
                    actorNode->addComponent<PhysicsComponent>();
                } catch (const std::exception& e) {
                    OHAO_LOG_WARNING("Could not add physics component: " + std::string(e.what()));
                }
            }
            ImGui::EndMenu();
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Rename")) {
            // Start rename operation
        }
        
        if (ImGui::MenuItem("Duplicate")) {
            if (currentScene) {
                auto duplicate = std::make_shared<Actor>(*actorNode);
                duplicate->setName(duplicate->getName() + " (Copy)");
                currentScene->addActor(duplicate);
            }
            ImGui::CloseCurrentPopup();
        }
        
        if (ImGui::MenuItem("Delete")) {
            handleObjectDeletion(node);
        }
        
        return;
    }
    
    // Legacy context menu for non-Actor nodes
    if (ImGui::MenuItem("Add Child")) {
        // Create child under selected node
        createPrimitiveObject(PrimitiveType::Empty);
    }

    if (!isRoot(node)) {  // Don't allow these operations on root
        if (ImGui::MenuItem("Rename")) {
            // Start rename operation
        }
        if (ImGui::MenuItem("Duplicate")) {
            auto sceneObj = dynamic_cast<SceneObject*>(node);
            if (sceneObj && currentScene) {
                auto duplicate = sceneObj->clone();
                duplicate->setName(duplicate->getName() + " (Copy)");
                
                // Create a new Actor with this data for the new system
                auto newActor = std::make_shared<Actor>(duplicate->getName());
                currentScene->addActor(newActor);
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Delete")) {
            handleObjectDeletion(node);
        }
    }
}

void OutlinerPanel::handleObjectDeletion(SceneNode* node) {
    if (!node || !currentScene) return;

    OHAO_LOG_DEBUG("Starting deletion of node: " + node->getName());

    // Check if this is an Actor first (new system)
    if (auto actor = dynamic_cast<Actor*>(node)) {
        // Clear selection first if this actor is selected
        if (SelectionManager::get().isSelected(actor)) {
            SelectionManager::get().clearSelection();
        }
        
        // Remove from scene's actors collection
        currentScene->removeActor(actor->getID());
        
        // Clear selection if this was the selected node
        if (selectedNode == node) {
            selectedNode = nullptr;
        }
        
        OHAO_LOG_DEBUG("Successfully deleted actor: " + actor->getName());
    }
    // Legacy system
    else {
        // Clear selection first if this object is selected
        if (auto sceneObj = asSceneObject(node)) {
            if (SelectionManager::get().isSelected(sceneObj)) {
                SelectionManager::get().clearSelection();
            }
            
            // Remove from scene's object collection
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
        
        OHAO_LOG_DEBUG("Successfully deleted node: " + node->getName());
    }

    // Update the buffers
    if (auto context = VulkanContext::getContextInstance()) {
        context->updateSceneBuffers();
    }
}

void OutlinerPanel::createPrimitiveObject(PrimitiveType type) {
    if (!currentScene) return;

    std::string objName;
    switch (type) {
        case PrimitiveType::Cube:
            objName = "Cube";
            break;
        case PrimitiveType::Sphere:
            objName = "Sphere";
            break;
        case PrimitiveType::Plane:
            objName = "Plane";
            break;
        case PrimitiveType::Cylinder:
            objName = "Cylinder";
            break;
        case PrimitiveType::Cone:
            objName = "Cone";
            break;
        case PrimitiveType::Empty:
        default:
            objName = "Empty";
            break;
    }

    // Create unique name
    int counter = 1;
    std::string baseName = objName;
    while (currentScene->findActor(objName) != nullptr) {
        objName = baseName + std::to_string(counter++);
    }

    // Create the actor with the given name
    auto newActor = currentScene->createActor(objName);
    
    // Generate and assign the mesh if it's not an empty object
    if (type != PrimitiveType::Empty && newActor) {
        // Add a mesh component
        auto meshComponent = newActor->addComponent<MeshComponent>();
        
        // Generate the appropriate mesh for this primitive type
        auto mesh = generatePrimitiveMesh(type);
        
        // Assign the mesh to the component
        if (meshComponent && mesh) {
            meshComponent->setModel(mesh);
            OHAO_LOG("Added " + objName + " with mesh component successfully");
        }
    }
    
    // Select the new object
    if (newActor) {
        SelectionManager::get().setSelectedActor(newActor.get());
        selectedNode = newActor.get();
    }

    // Update scene buffers to include the new object
    if (auto context = VulkanContext::getContextInstance()) {
        context->updateSceneBuffers();
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
    bool isSelected = (node == selectedNode);
    
    // Check if node is an Actor (new system)
    Actor* actorNode = dynamic_cast<Actor*>(node);

    // Different colors for root, selected, and regular nodes
    ImU32 nodeColor;
    if (isSelected) {
        nodeColor = IM_COL32(255, 165, 0, 255);  // Selected nodes are orange
    } else if (nodeIsRoot) {
        nodeColor = IM_COL32(100, 100, 100, 200);  // Root has slightly different appearance
    } else if (actorNode) {
        nodeColor = IM_COL32(120, 150, 200, 255);  // Actors have a blue tint
    } else {
        nodeColor = IM_COL32(100, 100, 100, 255);
    }

    float rounding = scaledNodeSize * 0.2f;

    // Draw node with different style based on type
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

    // Draw node label with different text for different node types
    ImVec2 textPos = nodePos;
    textPos.x += nodeSize.x * 0.5f;
    textPos.y += nodeSize.y + 5.0f;
    ImU32 textColor = nodeIsRoot ?
        IM_COL32(150, 150, 150, 255) :
        IM_COL32(255, 255, 255, 255);

    std::string label;
    if (nodeIsRoot) {
        label = "Root";
    } else if (actorNode) {
        // For Actor nodes, show the type
        label = node->getName() + " (Actor)";
    } else {
        label = node->getName();
    }
    
    drawList->AddText(
        textPos,
        textColor,
        label.c_str()
    );

    // Handle node selection
    ImVec2 mousePos = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(0)) {
        if (mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
            mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y) {
            selectedNode = node;
            if (actorNode) {
                // Use the new Actor selection system
                SelectionManager::get().setSelectedActor(actorNode);
            } else if (isSceneObject(node)) {
                // Legacy selection
                SelectionManager::get().setSelectedObject(asSceneObject(node));
            } else {
                SelectionManager::get().clearSelection();
            }
        }
    }

    // Calculate and draw connections to children
    float childSpacing = graphSpacingX * graphZoom;
    float verticalSpacing = graphSpacingY * graphZoom;

    // Handle different child traversal based on node type
    if (actorNode) {
        // For Actors, use the Actor-specific children list
        const auto& actorChildren = actorNode->getChildren();
        for (size_t i = 0; i < actorChildren.size(); ++i) {
            ImVec2 childPos = nodePos;
            childPos.x += childSpacing;
            childPos.y += verticalSpacing * (i - (actorChildren.size() - 1) * 0.5f);

            // Draw connection line
            drawList->AddLine(
                ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y * 0.5f),
                ImVec2(childPos.x, childPos.y + nodeSize.y * 0.5f),
                IM_COL32(150, 150, 150, 255),
                2.0f
            );

            // Render child node
            renderGraphNode(actorChildren[i], childPos, depth + 1);
        }
    } else {
        // Legacy SceneNode children
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
