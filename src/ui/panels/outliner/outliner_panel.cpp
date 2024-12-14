#include "outliner_panel.hpp"
#include "imgui.h"
#include "scene/scene_node.hpp"

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
        if (ImGui::MenuItem("Empty Object")) {
            if (currentScene) {
                auto newObject = std::make_shared<SceneObject>("New Object");
                currentScene->getRootNode()->addChild(newObject);
            }
        }
        if (ImGui::MenuItem("3D Model")) {
            // Add 3D model
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

} // namespace ohao
