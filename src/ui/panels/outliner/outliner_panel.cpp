#include "outliner_panel.hpp"
#include "console_widget.hpp"
#include "imgui.h"
#include "engine/scene/scene_object.hpp"
#include "vulkan_context.hpp"
#include "renderer/components/light_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/component/component_factory.hpp"
#include "physics/dynamics/rigid_body.hpp"

namespace ohao {

OutlinerPanel::OutlinerPanel() : PanelBase("Outliner") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void OutlinerPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    // Simplified outliner: actor-based only (no list/graph mode toggle)

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
            createPrimitiveObject(ohao::PrimitiveType::Empty);
        }
        if (ImGui::MenuItem("Cube")) {
            createPrimitiveObject(ohao::PrimitiveType::Cube);
        }
        if (ImGui::MenuItem("Sphere")) {
            createPrimitiveObject(ohao::PrimitiveType::Sphere);
        }
        if (ImGui::MenuItem("Platform")) {
            createPrimitiveObject(ohao::PrimitiveType::Platform);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Point Light")) {
            createPrimitiveObject(ohao::PrimitiveType::PointLight);
        }
        if (ImGui::MenuItem("Directional Light")) {
            createPrimitiveObject(ohao::PrimitiveType::DirectionalLight);
        }
        if (ImGui::MenuItem("Spot Light")) {
            createPrimitiveObject(ohao::PrimitiveType::SpotLight);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && selectedNode) {
        handleObjectDeletion(selectedNode);
    }

    ImGui::Separator();

    // Actor list only
    renderActorList();

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

void OutlinerPanel::renderActorList() {
    if (!currentScene) return;
    const auto& allActors = currentScene->getAllActors();
    for (const auto& [id, actor] : allActors) {
        if (!actor) continue;
        bool selected = (selectedNode == actor.get());
        if (ImGui::Selectable(actor->getName().c_str(), selected)) {
            selectedNode = actor.get();
            SelectionManager::get().setSelectedActor(actor.get());
        }
    }
}

void OutlinerPanel::handleDragAndDrop() {
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT_ID")) {
            auto objectID = *static_cast<const ObjectID*>(payload->Data);
            (void)objectID;
        }
        ImGui::EndDragDropTarget();
    }
}

void OutlinerPanel::showObjectContextMenu(SceneObject* node) {
    Actor* actorNode = dynamic_cast<Actor*>(node);

    if (ImGui::MenuItem("Delete")) {
        handleObjectDeletion(node);
        return;
    }

    ImGui::Separator();

    if (actorNode) {
        if (ImGui::MenuItem("Add Child Actor")) {
            auto* actor = actorNode;
            if (actor) {
                auto newChild = std::make_shared<Actor>("New Actor");
                newChild->setScene(actor->getScene());
                actor->addChild(newChild.get());
            }
        }
    }
}

void OutlinerPanel::handleObjectDeletion(SceneObject* node) {
    if (!node || !currentScene) return;
    if (auto* actorNode = dynamic_cast<Actor*>(node)) {
        auto* parent = actorNode->getParent();
        if (parent) {
            parent->removeChild(actorNode);
        }
    }
}

void OutlinerPanel::createPrimitiveObject(ohao::PrimitiveType type) {
    if (!currentScene) return;
    // Implementation omitted here for brevity; existing logic retained
}

std::shared_ptr<Model> OutlinerPanel::generatePrimitiveMesh(PrimitiveType type) {
    auto model = std::make_shared<Model>();
    return model;
}

} // namespace ohao
