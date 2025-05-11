#include "outliner_panel.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <memory>   // For std::bad_weak_ptr
#include <stdexcept>
#include <typeindex>
#include <typeinfo>
#include <unordered_set>
#include "scene/scene_node.hpp"
#include "vulkan_context.hpp"
#include "core/actor/light_actor.hpp"

namespace ohao {

OutlinerPanel::OutlinerPanel() : PanelBase("Outliner") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void OutlinerPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    renderViewModeSelector();

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        handleDelete();
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
        
        // Add light submenu
        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Point Light")) {
                createLightActor(LightActor::LightActorType::Point);
            }
            if (ImGui::MenuItem("Directional Light")) {
                createLightActor(LightActor::LightActorType::Directional);
            }
            if (ImGui::MenuItem("Spot Light")) {
                createLightActor(LightActor::LightActorType::Spot);
            }
            if (ImGui::MenuItem("Area Light")) {
                createLightActor(LightActor::LightActorType::Area);
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && selectedNode) {
        handleDelete();
    }

    ImGui::Separator();

    // Scene Tree
    if (currentViewMode == ViewMode::List) {
        renderSceneTree();
    } else {
        renderGraphView();
    }

    // Process global drag and drop context
    if (ImGui::BeginDragDropTarget()) {
        // Log the drag and drop attempt
        OHAO_LOG_DEBUG("Global drag and drop detected in outliner panel");
        ImGui::EndDragDropTarget();
    }

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
    if (!currentScene) {
        return;
    }
    
    try {
        // Get all actors from the scene - in a non-hierarchical model, all actors are top-level
        const auto& allActors = currentScene->getAllActors();
        if (allActors.empty()) {
            return;
        }
        
        // Render all actors
        for (const auto& [id, actor] : allActors) {
            // Skip null actors
            if (!actor) {
                continue;
            }
            
            try {
                ImGui::PushID(static_cast<void*>(actor.get()));
                renderTreeNode(actor.get());
                ImGui::PopID();
            } catch (const std::exception& e) {
                OHAO_LOG_ERROR("Exception rendering actor: " + std::string(e.what()));
            }
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Exception in renderSceneTree: " + std::string(e.what()));
    }
}

void OutlinerPanel::renderOrphanedActors() {
    // No-op in non-hierarchical model as all actors are displayed at the top level
    return;
}

void OutlinerPanel::renderTreeNode(SceneNode* node) {
    if (!node) {
        return;
    }
    
    // First, verify that node is not corrupted by trying to access a safe method
    try {
        // Basic verification - just try accessing name (most basic SceneNode operation)
        std::string nodeName = node->getName(); 
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Invalid SceneNode pointer encountered: " + std::string(e.what()));
        return; // Abort rendering this branch
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                              ImGuiTreeNodeFlags_OpenOnDoubleClick |
                              ImGuiTreeNodeFlags_SpanAvailWidth;

    // Allow selection for all nodes
    if (node == selectedNode) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Check if this is a leaf node (no children) - set as leaf in the tree display
    bool isLeaf = true;
    
    // Check if this is an Actor - use careful type checking to avoid crashes
    Actor* actorNode = nullptr;
    bool isActorValid = false;
    
    // We can't directly rely on pointer type information, so use a multi-step validation approach
    try {
        // First verify with typeid - this is safer than immediate dynamic_cast
        const std::type_info& nodeType = typeid(*node);
        if (nodeType == typeid(Actor) || 
            nodeType.before(typeid(Actor)) == false) { // Check if it's Actor or derived from Actor
            
            // Only now attempt the dynamic_cast with known valid type
            actorNode = dynamic_cast<Actor*>(node);
            isActorValid = (actorNode != nullptr);
            
            // For valid actors, check if they have children to determine leaf status
            if (isActorValid) {
                try {
                    // Explicit try-catch around children access
                    const auto& children = actorNode->getChildren();
                    isLeaf = children.empty();
                } catch (const std::exception& e) {
                    OHAO_LOG_ERROR("Error accessing actor children: " + std::string(e.what()));
                    isLeaf = true; // Assume no children if we can't access them
                }
            }
        }
    } catch (const std::bad_typeid& e) {
        OHAO_LOG_ERROR("Type identification error: " + std::string(e.what()));
        return; // Abort rendering this node
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error checking node type: " + std::string(e.what()));
        return; // Abort rendering this node
    }
    
    // Set leaf flag if this is a leaf node
    if (isLeaf) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    // Determine display name and icon
    std::string displayName;
    std::string nodeTypeText = "";
    
    try {
        // Get the name safely with fallback
        displayName = node->getName();
        if (displayName.empty()) {
            displayName = "Unnamed";
        }
        
        // For actors, check if we have primitive_type metadata to improve the display
        if (isActorValid && actorNode) {
            // Get primitive type if available
            std::string primitiveType = "";
            if (actorNode->hasMetadata("primitive_type")) {
                primitiveType = actorNode->getMetadata("primitive_type");
                nodeTypeText = " (" + primitiveType + ")";
            } else {
                // Try to determine type from components
                auto meshComp = actorNode->getComponent<MeshComponent>();
                if (meshComp && meshComp->getModel()) {
                    nodeTypeText = " (Mesh)";
                } else if (dynamic_cast<LightActor*>(actorNode)) {
                    nodeTypeText = " (Light)";
                } else {
                    nodeTypeText = " (Actor)";
                }
            }
            
            // Combine name with type
            displayName = displayName + nodeTypeText;
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error getting node name: " + std::string(e.what()));
        displayName = "Invalid Node";
    }
    
    // Actually render the tree node with the name
    bool nodeOpen = false;
    try {
        nodeOpen = ImGui::TreeNodeEx(displayName.c_str(), flags);
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error rendering tree node: " + std::string(e.what()));
        return; // If we can't render the node itself, abort
    }
    
    // Handle drag and drop
    try {
        if (isActorValid && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SCENE_OBJECT", &actorNode, sizeof(Actor*));
            ImGui::Text("Moving %s", displayName.c_str());
            ImGui::EndDragDropSource();
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error handling drag source: " + std::string(e.what()));
        // Continue without crashing
    }
    
    if (ImGui::BeginDragDropTarget()) {
        try {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
                // Handle the drop
                handleDragAndDrop(node, payload);
            }
        } catch (const std::exception& e) {
            OHAO_LOG_ERROR("Error handling drag target: " + std::string(e.what()));
        }
        ImGui::EndDragDropTarget();
    }
    
    // Handle clicks to select the node
    if (ImGui::IsItemClicked()) {
        selectedNode = node;
        
        // Update the selection in the selection manager based on node type
        if (isActorValid) {
            SelectionManager::get().setSelectedActor(actorNode);
        } else if (SceneObject* sceneObj = asSceneObject(node)) {
            SelectionManager::get().setSelectedObject(sceneObj);
        } else {
            SelectionManager::get().clearSelection();
        }
    }
    
    // Handle right-click context menu
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        selectedNode = node;
        contextMenuTarget = node;
        showContextMenu = true;
        
        // Also update selection
        if (isActorValid) {
            SelectionManager::get().setSelectedActor(actorNode);
        } else if (SceneObject* sceneObj = asSceneObject(node)) {
            SelectionManager::get().setSelectedObject(sceneObj);
        }
    }
    
    // Render children if the node is open
    if (nodeOpen) {
        if (isActorValid) {
            // Safely show children for Actor nodes
            try {
                const auto& children = actorNode->getChildren();
                for (Actor* child : children) {
                    if (child) {
                        try {
                            // Try to do a basic verification that child is valid before rendering
                            const std::string childName = child->getName();
                            const ObjectID childId = child->getID();
                            
                            ImGui::PushID(static_cast<void*>(child)); // Use pointer as unique ID
                            renderTreeNode(child);
                            ImGui::PopID();
                        } catch (const std::exception& e) {
                            OHAO_LOG_ERROR("Invalid child pointer encountered: " + std::string(e.what()));
                            // Skip this child but continue with others
                        }
                    }
                }
            } catch (const std::exception& e) {
                OHAO_LOG_ERROR("Error rendering actor children: " + std::string(e.what()));
                // Continue without crashing
            }
        }
        ImGui::TreePop();
    }
}

void OutlinerPanel::handleGlobalDragAndDrop() {
    // This method remains as a legacy handler, but we should avoid using it
    OHAO_LOG_WARNING("Deprecated handleGlobalDragAndDrop called - should use node-specific drag handling");
    
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
                        // Check if the selected node is an Actor - safely
                        Actor* targetActor = nullptr;
                        try {
                            targetActor = dynamic_cast<Actor*>(selectedNode);
                        } catch (const std::exception& e) {
                            // Not an Actor, will use fallback
                        }
                        
                        if (targetActor) {
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
                // Check if this is an Actor - safely
                Actor* actorNode = nullptr;
                try {
                    actorNode = dynamic_cast<Actor*>(droppedNode);
                } catch (const std::exception& e) {
                    // Not an Actor
                }
                
                // Check if target is an Actor - safely
                Actor* targetActor = nullptr;
                try {
                    targetActor = dynamic_cast<Actor*>(selectedNode);
                } catch (const std::exception& e) {
                    // Not an Actor
                }
                
                if (actorNode && targetActor) {
                    // Actor-to-Actor parenting
                    actorNode->setParent(targetActor);
                } else {
                    // Legacy fallback with safety checks
                    try {
                        droppedNode->detachFromParent();
                        selectedNode->addChild(std::shared_ptr<SceneNode>(droppedNode));
                    } catch (const std::exception& e) {
                        OHAO_LOG_WARNING("Failed to reparent node: " + std::string(e.what()));
                    }
                }
                OHAO_LOG_DEBUG("Reparented node: " + droppedNode->getName() + 
                            " to parent: " + selectedNode->getName());
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void OutlinerPanel::showObjectContextMenu(SceneNode* node) {
    if (!node || !currentScene) return;
    
    // Check if this is an Actor - use careful type checking to avoid crashes
    Actor* actorNode = nullptr;
    try {
        actorNode = dynamic_cast<Actor*>(node);
    } catch (const std::exception& e) {
        // Not an Actor
    }
    
    // New Actor-based context menu
    if (actorNode) {
        // Check if it's a light actor for specialized menu items
        bool isLightActor = false;
        LightActor* lightActor = nullptr;
        try {
            lightActor = dynamic_cast<LightActor*>(actorNode);
            isLightActor = (lightActor != nullptr);
        } catch (const std::exception& e) {
            // Not a light actor
        }
        
        if (isLightActor && lightActor) {
            // Show light type next to name
            std::string lightTypeStr;
            switch (lightActor->getLightType()) {
                case LightType::Point: lightTypeStr = "Point Light"; break;
                case LightType::Directional: lightTypeStr = "Directional Light"; break;
                case LightType::Spot: lightTypeStr = "Spot Light"; break;
                case LightType::Area: lightTypeStr = "Area Light"; break;
                default: lightTypeStr = "Unknown Light Type"; break;
            }
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s (%s)", lightActor->getName().c_str(), lightTypeStr.c_str());
        } else {
            // Regular actor display
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%s", actorNode->getName().c_str());
        }
        
        ImGui::Separator();
        
        if (ImGui::BeginMenu("Add Component")) {
            if (ImGui::MenuItem("Mesh Component")) {
                try {
                    actorNode->addComponent<MeshComponent>();
                } catch (const std::exception& e) {
                    OHAO_LOG_WARNING("Could not add mesh component: " + std::string(e.what()));
                }
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
            handleDelete();
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
            // Safe access to SceneObject
            SceneObject* sceneObj = nullptr;
            try {
                sceneObj = dynamic_cast<SceneObject*>(node);
            } catch (const std::exception& e) {
                // Not a SceneObject
            }
            
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
            handleDelete();
        }
    }
}

void OutlinerPanel::handleDelete() {
    if (selectedNode) {
        handleObjectDeletion(selectedNode);
        selectedNode = nullptr; // Safely clear selected node after deletion
    }
}

void OutlinerPanel::handleObjectDeletion(SceneNode* node) {
    if (!node || !currentScene) return;

    OHAO_LOG_DEBUG("Starting deletion of node: " + node->getName());

    // Check if this is an Actor first (new system) - safely
    Actor* actor = nullptr;
    try {
        actor = dynamic_cast<Actor*>(node);
    } catch (const std::exception& e) {
        // Not an Actor
    }
    
    if (actor) {
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
        SceneObject* sceneObj = asSceneObject(node);
        if (sceneObj) {
            if (SelectionManager::get().isSelected(sceneObj)) {
                SelectionManager::get().clearSelection();
            }
            
            // Remove from scene's object collection
            currentScene->removeObject(sceneObj->getName());
        }

        // Remove from scene hierarchy
        if (node->getParent()) {
            try {
                node->detachFromParent();
            } catch (const std::exception& e) {
                // Log error but continue with deletion
                OHAO_LOG_WARNING("Error detaching node from parent: " + std::string(e.what()));
            }
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

    // Map primitive type to a descriptive base name
    std::string baseName;
    switch (type) {
        case PrimitiveType::Cube:
            baseName = "Cube";
            break;
        case PrimitiveType::Sphere:
            baseName = "Sphere";
            break;
        case PrimitiveType::Plane:
            baseName = "Plane";
            break;
        case PrimitiveType::Cylinder:
            baseName = "Cylinder";
            break;
        case PrimitiveType::Cone:
            baseName = "Cone";
            break;
        case PrimitiveType::Empty:
        default:
            baseName = "Empty";
            break;
    }

    // Generate a unique name with numeric suffix
    std::string objectName = generateUniqueName(baseName);

    // Create the actor with the generated name
    auto newActor = currentScene->createActor(objectName);
    
    // Add metadata to track the primitive type for serialization
    if (newActor) {
        newActor->setMetadata("primitive_type", baseName);
    }
    
    // Generate and assign the mesh if it's not an empty object
    if (type != PrimitiveType::Empty && newActor) {
        // Add a mesh component
        auto meshComponent = newActor->addComponent<MeshComponent>();
        
        // Generate the appropriate mesh for this primitive type
        auto mesh = generatePrimitiveMesh(type);
        
        // Assign the mesh to the component
        if (meshComponent && mesh) {
            meshComponent->setModel(mesh);
            OHAO_LOG("Added " + objectName + " with mesh component successfully");
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

// Helper method to generate unique names with incrementing counters
std::string OutlinerPanel::generateUniqueName(const std::string& baseName) {
    // Use a static map to track counters for each base name
    static std::unordered_map<std::string, int> counters;
    
    // Get the next counter value for this base name
    int counter = ++counters[baseName];
    
    // Create the name with suffix
    std::string uniqueName = baseName + "_" + std::to_string(counter);
    
    // Ensure it's truly unique in the scene
    if (currentScene) {
        int attemptCount = 0;
        while (currentScene->findActor(uniqueName) != nullptr && attemptCount < 1000) {
            counter = ++counters[baseName];
            uniqueName = baseName + "_" + std::to_string(counter);
            attemptCount++;
        }
    }
    
    return uniqueName;
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
    if (!currentScene) return;

    // Create a child window for the graph
    ImGui::BeginChild("GraphView", ImVec2(0, 0), true,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

    handleGraphNavigation();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    // Start position for actors
    ImVec2 startPos = canvasPos;
    startPos.x += graphOffset.x;
    startPos.y += graphOffset.y;

    // In non-hierarchical structure, we render all actors at the same level
    // Collect all actors from the scene
    const auto& allActors = currentScene->getAllActors();
    
    // Calculate positions for all actors in a grid layout
    float scaledNodeSize = graphNodeSize * graphZoom;
    float nodePadding = 20.0f * graphZoom;
    int nodesPerRow = std::max(1, static_cast<int>(std::sqrt(allActors.size())));
    
    int actorIndex = 0;
    for (const auto& [id, actor] : allActors) {
        if (!actor) continue;
        
        // Calculate grid position
        int row = actorIndex / nodesPerRow;
        int col = actorIndex % nodesPerRow;
        
        ImVec2 nodePos = startPos;
        nodePos.x += col * (scaledNodeSize + nodePadding);
        nodePos.y += row * (scaledNodeSize + nodePadding * 2);
        
        // Render the node
        renderActorNode(actor.get(), nodePos);
        
        actorIndex++;
    }

    ImGui::EndChild();
}

void OutlinerPanel::renderActorNode(Actor* actor, ImVec2& pos) {
    if (!actor) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 nodePos = pos;

    // Calculate node size based on zoom
    float scaledNodeSize = graphNodeSize * graphZoom;
    ImVec2 nodeSize(scaledNodeSize, scaledNodeSize);

    bool isSelected = (actor == selectedNode);
    
    // Different colors for selected and regular nodes
    ImU32 nodeColor;
    if (isSelected) {
        nodeColor = IM_COL32(255, 165, 0, 255);  // Selected nodes are orange
    } else {
        nodeColor = IM_COL32(120, 150, 200, 255);  // Actors have a blue tint
    }

    float rounding = scaledNodeSize * 0.2f;

    // Draw the node
    drawList->AddRectFilled(
        nodePos,
        ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
        nodeColor,
        rounding
    );

    // Draw node label
    ImVec2 textPos = nodePos;
    textPos.x += nodeSize.x * 0.5f;
    textPos.y += nodeSize.y + 5.0f;
    ImU32 textColor = IM_COL32(255, 255, 255, 255);

    std::string label = actor->getName();
    
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
            selectedNode = actor;
            SelectionManager::get().setSelectedActor(actor);
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
    if (!node || !currentScene) return false;
    
    try {
        // First, check if the node itself is valid by trying to access a basic property
        try {
            const std::string& nodeName = node->getName();
            if (nodeName.empty()) {
                OHAO_LOG_DEBUG("Node has empty name - unlikely to be root");
            }

            // If we can get the name, the node is at least partially valid
        } catch (const std::exception& e) {
            OHAO_LOG_DEBUG("Failed to validate node in isRoot: " + std::string(e.what()));
            return false;
        }
        
        // Safely get the root node
        std::shared_ptr<Actor> rootNode;
        
        try {
            // Handle potential exceptions when getting the root node
            rootNode = currentScene->getRootNode();
        } catch (const std::bad_weak_ptr& e) {
            // Common when accessing expired shared_from_this()
            OHAO_LOG_WARNING("Bad weak pointer in isRoot check: " + std::string(e.what()));
            return false;
        } catch (const std::exception& e) {
            OHAO_LOG_WARNING("Exception in isRoot check: " + std::string(e.what()));
            return false;
        }
        
        // Check if the rootNode is valid
        if (!rootNode) {
            OHAO_LOG_DEBUG("Scene has no root node");
            return false;
        }
        
        // Check if root node pointer matches the node we're checking
        if (rootNode.get() == node) {
            return true;
        }
        
        // As a secondary check, compare IDs if the node is an Actor
        try {
            if (dynamic_cast<Actor*>(node)) {
                Actor* actorNode = static_cast<Actor*>(node);
                ObjectID nodeId = actorNode->getID();
                ObjectID rootId = rootNode->getID();
                
                // Root node should have ID 1
                if (nodeId == 1 && nodeId == rootId) {
                    return true;
                }

                // Log potential ID issues
                if (nodeId == 1 && rootId != 1) {
                    OHAO_LOG_WARNING("Node has ID 1 but root node has ID " + std::to_string(rootId));
                }
            }
        } catch (const std::exception& e) {
            OHAO_LOG_WARNING("Exception in ID check for root: " + std::string(e.what()));
        }
        
        return false;
    } catch (const std::exception& e) {
        OHAO_LOG_WARNING("Exception in isRoot check: " + std::string(e.what()));
        return false;
    }
}

bool OutlinerPanel::isSceneObject(SceneNode* node) const {
    if (!node) return false;
    
    // Use a safer dynamic_cast with try-catch to avoid crashes
    try {
        // First verify node is a valid pointer by checking typeid
        const std::type_info& nodeType = typeid(*node);
        
        // First try Actor - which is most common in new system
        if (nodeType == typeid(Actor) || 
            !nodeType.before(typeid(Actor))) { // Derived from Actor
            
            // Now try safe cast to Actor
            if (Actor* actor = dynamic_cast<Actor*>(node)) {
                return true; // Actor is a SceneObject
            }
        }
        
        // Next check if it's directly a SceneObject or derived
        if (nodeType == typeid(SceneObject) || 
            !nodeType.before(typeid(SceneObject))) {
            
            return dynamic_cast<SceneObject*>(node) != nullptr;
        }
        
        // If we get here, it's not a valid SceneObject type
        return false;
    } 
    catch (const std::bad_typeid& e) {
        // Specific error for corrupted RTTI
        OHAO_LOG_WARNING("Failed to identify node type: " + std::string(e.what()));
        return false;
    }
    catch (const std::exception& e) {
        OHAO_LOG_WARNING("Failed to cast node to SceneObject: " + std::string(e.what()));
        return false;
    }
}

bool OutlinerPanel::isSelected(SceneNode* node) const {
    return node == selectedNode;
}

void OutlinerPanel::createLightActor(LightActor::LightActorType type) {
    if (!currentScene) return;
    
    // Determine a descriptive name based on light type
    std::string baseName;
    switch (type) {
        case LightActor::LightActorType::Point:
            baseName = "PointLight";
            break;
        case LightActor::LightActorType::Directional:
            baseName = "DirectionalLight";
            break;
        case LightActor::LightActorType::Spot:
            baseName = "SpotLight";
            break;
        case LightActor::LightActorType::Area:
            baseName = "AreaLight";
            break;
    }
    
    // Create unique name
    int counter = 1;
    std::string lightName = baseName;
    while (currentScene->getActor(lightName) != nullptr) {
        lightName = baseName + std::to_string(counter++);
    }
    
    // Create and add the light actor
    auto lightActor = LightActor::createLight(lightName, type);
    currentScene->addActor(lightActor);
    
    // Select the new light
    selectedNode = lightActor.get();
    SelectionManager::get().setSelectedActor(lightActor.get());
    
    // Update scene buffers
    if (auto context = VulkanContext::getContextInstance()) {
        context->updateSceneBuffers();
    }
    
    OHAO_LOG("Added " + lightName + " to scene");
}

SceneObject* OutlinerPanel::asSceneObject(SceneNode* node) const {
    if (!node) return nullptr;
    
    // Multi-step validation to prevent crashes during dynamic_cast
    try {
        // First verify node is a valid pointer by checking typeid
        const std::type_info& nodeType = typeid(*node);
        
        // First try Actor - which is most common in new system
        if (nodeType == typeid(Actor) || 
            !nodeType.before(typeid(Actor))) { // Derived from Actor
            
            // Now try safe cast to Actor
            if (Actor* actor = dynamic_cast<Actor*>(node)) {
                return actor; // Actor is a SceneObject
            }
        }
        
        // Next check if it's directly a SceneObject or derived
        if (nodeType == typeid(SceneObject) || 
            !nodeType.before(typeid(SceneObject))) {
            
            return dynamic_cast<SceneObject*>(node);
        }
        
        // If we get here, it's not a valid SceneObject type
        return nullptr;
    } 
    catch (const std::bad_typeid& e) {
        // Specific error for corrupted RTTI
        OHAO_LOG_WARNING("Failed to identify node type: " + std::string(e.what()));
        return nullptr;
    }
    catch (const std::exception& e) {
        OHAO_LOG_WARNING("Failed to cast node to SceneObject: " + std::string(e.what()));
        return nullptr;
    }
}

void OutlinerPanel::handleDragAndDrop(SceneNode* targetNode, const ImGuiPayload* payload) {
    if (!targetNode || !payload || !payload->Data || !currentScene) return;
    
    try {
        // Check if the target is an Actor 
        Actor* targetActor = dynamic_cast<Actor*>(targetNode);
        if (!targetActor) {
            OHAO_LOG_WARNING("Drop target is not an Actor");
            return;
        }
        
        // Check what type of data we're receiving
        if (strcmp(payload->DataType, "SCENE_OBJECT") == 0) {
            // We're dragging a scene object
            Actor** sourcePtr = static_cast<Actor**>(payload->Data);
            if (!sourcePtr || !*sourcePtr) {
                OHAO_LOG_WARNING("Invalid source actor pointer in drag data");
                return;
            }
            
            Actor* sourceActor = *sourcePtr;
            
            // Make sure we're not trying to parent an object to itself or its child
            if (sourceActor == targetActor) {
                OHAO_LOG_WARNING("Cannot parent an object to itself");
                return;
            }
            
            // Check for circular references (if target is a child of source)
            Actor* checkNode = targetActor->getParent();
            while (checkNode) {
                if (checkNode == sourceActor) {
                    OHAO_LOG_WARNING("Cannot create circular parent references");
                    return;
                }
                checkNode = checkNode->getParent();
            }
            
            // Set the parent
            sourceActor->setParent(targetActor);
            OHAO_LOG("Parented " + sourceActor->getName() + " to " + targetActor->getName());
        }
    } catch (const std::exception& e) {
        OHAO_LOG_ERROR("Error in drag and drop: " + std::string(e.what()));
    }
}

} // namespace ohao
