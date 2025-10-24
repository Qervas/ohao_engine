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
        if (selectedObject) {
            handleObjectDeletion(selectedObject);
        }
    }

    // Toolbar
    if (ImGui::Button("Add")) {
        ImGui::OpenPopup("AddObjectPopup");
    }

    if (ImGui::BeginPopup("AddObjectPopup")) {
        auto addPrimitive = [&](const char* label, ohao::PrimitiveType type){
            if (ImGui::MenuItem(label)) {
                if (currentScene) {
                    currentScene->createActorWithComponents(label, type);
                    currentScene->updateSceneBuffers();
                }
            }
        };
        addPrimitive("Empty", ohao::PrimitiveType::Empty);
        addPrimitive("Cube", ohao::PrimitiveType::Cube);
        addPrimitive("Sphere", ohao::PrimitiveType::Sphere);
        addPrimitive("Platform", ohao::PrimitiveType::Platform);
        ImGui::Separator();
        addPrimitive("Point Light", ohao::PrimitiveType::PointLight);
        addPrimitive("Directional Light", ohao::PrimitiveType::DirectionalLight);
        addPrimitive("Spot Light", ohao::PrimitiveType::SpotLight);
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete") && selectedObject) {
        handleObjectDeletion(selectedObject);
    }

    ImGui::Separator();

    // Actor list only
    renderActorList();

    handleDragAndDrop();

    if (showContextMenu) {
        ImGui::OpenPopup("ObjectContextMenu");
        showContextMenu = false;
    }

    if (ImGui::BeginPopup("ObjectContextMenu") && contextMenuTargetObject) {
        showObjectContextMenu(contextMenuTargetObject);
        ImGui::EndPopup();
    }

    // Apply deferred deletions safely outside of ImGui popup building
    if (pendingDeleteActor && currentScene) {
        auto toRemove = currentScene->findActor(pendingDeleteActor->getID());
        if (toRemove) {
            currentScene->removeActor(toRemove);
            SelectionManager::get().clearSelection();
            selectedObject = nullptr;
            currentScene->updateSceneBuffers();
            OHAO_LOG("Actor deleted: " + toRemove->getName());
        }
        pendingDeleteActor = nullptr;
    }

    ImGui::End();
}

void OutlinerPanel::renderActorList() {
    if (!currentScene) return;
    // Header root label (non-selectable)
    ImGui::TextUnformatted("World");
    ImGui::Separator();

    const auto& allActors = currentScene->getAllActors();
    Actor* root = currentScene->getRootNode() ? currentScene->getRootNode().get() : nullptr;

    // Show only top-level actors (no parent), and hide the artificial root
    for (const auto& [id, actor] : allActors) {
        if (!actor) continue;
        if (actor.get() == root) continue; // hide root
        if (actor->getParent() != nullptr) continue; // only top-level here

        bool selected = (selectedObject == actor.get());
        if (ImGui::Selectable(actor->getName().c_str(), selected)) {
            selectedObject = actor.get();
            SelectionManager::get().setSelectedActor(actor.get());
        }

        // Context menu on item
        if (ImGui::BeginPopupContextItem()) {
            contextMenuTargetObject = actor.get();
            showObjectContextMenu(contextMenuTargetObject);
            ImGui::EndPopup();
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

void OutlinerPanel::showObjectContextMenu(SceneObject* object) {
    Actor* actorNode = dynamic_cast<Actor*>(object);

    // Prevent deleting root
    Actor* root = currentScene && currentScene->getRootNode() ? currentScene->getRootNode().get() : nullptr;
    bool isRoot = (actorNode && actorNode == root);

    if (!isRoot) {
        if (ImGui::MenuItem("Delete")) {
            // Defer deletion until after popup ends to avoid invalidating iterators/UI
            pendingDeleteActor = actorNode;
        }
    } else {
        ImGui::MenuItem("Delete", nullptr, false, false);
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

void OutlinerPanel::handleObjectDeletion(SceneObject* object) {
    if (!object || !currentScene) return;
    if (auto* actorNode = dynamic_cast<Actor*>(object)) {
        // Defer actual removal to main render loop (handled after popup)
        pendingDeleteActor = actorNode;
    }
}

void OutlinerPanel::createPrimitiveObject(ohao::PrimitiveType type) {
    if (!currentScene) return;
    // Deprecated path: use Scene::createActorWithComponents instead
    currentScene->createActorWithComponents("Actor", type);
    currentScene->updateSceneBuffers();
}

std::shared_ptr<Model> OutlinerPanel::generatePrimitiveMesh(PrimitiveType type) {
    auto model = std::make_shared<Model>();
    return model;
}

} // namespace ohao
