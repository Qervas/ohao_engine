#include "physics_component_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "console_widget.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"

namespace ohao {

PhysicsComponentPanel::PhysicsComponentPanel() : PanelBase("Physics Component") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void PhysicsComponentPanel::render() {
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
            auto physicsComponent = m_selectedActor->getComponent<PhysicsComponent>();
            if (physicsComponent) {
                renderPhysicsProperties(physicsComponent.get());
            } else {
                ImGui::TextDisabled("No PhysicsComponent found on selected actor");
            }
        } else {
            ImGui::TextDisabled("No actor selected");
        }
    }

    if (!isInChildWindow) {
        ImGui::End();
    }
}

void PhysicsComponentPanel::renderPhysicsProperties(PhysicsComponent* component) {
    if (!component) return;

    ImGui::Text("Physics Component Properties");
    ImGui::Separator();

    // === Basic Physics Properties ===
    if (ImGui::CollapsingHeader("Basic Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Rigid Body Type
        const char* rigidBodyTypeNames[] = { "Static", "Kinematic", "Dynamic" };
        int currentType = static_cast<int>(component->getRigidBodyType());
        if (ImGui::Combo("Rigid Body Type", &currentType, rigidBodyTypeNames, 3)) {
            component->setRigidBodyType(static_cast<physics::dynamics::RigidBodyType>(currentType));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Static: Never moves (ground, walls)\nKinematic: Moves but not affected by forces\nDynamic: Full physics simulation");
        }

        // Mass (only for dynamic bodies)
        if (component->getRigidBodyType() == physics::dynamics::RigidBodyType::DYNAMIC) {
            float mass = component->getMass();
            if (ImGui::DragFloat("Mass", &mass, 0.1f, 0.01f, 1000.0f, "%.2f kg")) {
                component->setMass(mass);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Mass affects how the object responds to forces");
            }
        } else {
            ImGui::TextDisabled("Mass: Infinite (Static/Kinematic)");
        }

        // Gravity toggle
        bool gravityEnabled = component->isGravityEnabled();
        if (ImGui::Checkbox("Gravity Enabled", &gravityEnabled)) {
            component->setGravityEnabled(gravityEnabled);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Whether this object is affected by gravity");
        }
    }

    // === Material Properties ===
    if (ImGui::CollapsingHeader("Material Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Friction
        float friction = component->getFriction();
        if (ImGui::SliderFloat("Friction", &friction, 0.0f, 2.0f, "%.3f")) {
            component->setFriction(friction);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Surface friction (0 = slippery, 1 = normal, >1 = grippy)");
        }

        // Restitution (Bounciness)
        float restitution = component->getRestitution();
        if (ImGui::SliderFloat("Restitution", &restitution, 0.0f, 1.0f, "%.3f")) {
            component->setRestitution(restitution);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bounciness (0 = no bounce, 1 = perfect bounce)");
        }

        // Linear Damping
        float linearDamping = component->getLinearDamping();
        if (ImGui::SliderFloat("Linear Damping", &linearDamping, 0.0f, 1.0f, "%.3f")) {
            component->setLinearDamping(linearDamping);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Air resistance for linear motion (0 = no damping, 1 = high damping)");
        }

        // Angular Damping
        float angularDamping = component->getAngularDamping();
        if (ImGui::SliderFloat("Angular Damping", &angularDamping, 0.0f, 1.0f, "%.3f")) {
            component->setAngularDamping(angularDamping);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Air resistance for rotational motion (0 = no damping, 1 = high damping)");
        }
    }

    // === Velocity Controls ===
    if (ImGui::CollapsingHeader("Velocity & Forces")) {
        // Linear Velocity
        glm::vec3 linearVel = component->getLinearVelocity();
        if (renderVec3Control("Linear Velocity", linearVel, 0.0f)) {
            component->setLinearVelocity(linearVel);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Current velocity in world space (m/s)");
        }

        // Angular Velocity
        glm::vec3 angularVel = component->getAngularVelocity();
        if (renderVec3Control("Angular Velocity", angularVel, 0.0f)) {
            component->setAngularVelocity(angularVel);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Current angular velocity (rad/s)");
        }

        // Force Application
        ImGui::Separator();
        ImGui::Text("Apply Forces:");

        static glm::vec3 forceToApply{0.0f, 0.0f, 0.0f};
        renderVec3Control("Force", forceToApply, 0.0f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 0.8f));
        if (ImGui::Button("Apply Force", ImVec2(100, 25))) {
            component->applyForce(forceToApply);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 0.8f));
        if (ImGui::Button("Clear Forces", ImVec2(100, 25))) {
            component->clearForces();
        }
        ImGui::PopStyleColor();

        // Quick force presets
        ImGui::Text("Quick Forces:");
        if (ImGui::Button("Jump (+Y)", ImVec2(60, 20))) {
            component->applyForce(glm::vec3(0.0f, 500.0f, 0.0f));
        }
        ImGui::SameLine();
        if (ImGui::Button("Push (+X)", ImVec2(60, 20))) {
            component->applyForce(glm::vec3(100.0f, 0.0f, 0.0f));
        }
        ImGui::SameLine();
        if (ImGui::Button("Push (+Z)", ImVec2(60, 20))) {
            component->applyForce(glm::vec3(0.0f, 0.0f, 100.0f));
        }
    }

    // === Collision Shape ===
    if (ImGui::CollapsingHeader("Collision Shape")) {
        auto collisionShape = component->getCollisionShape();
        if (collisionShape) {
            // Show shape type and details
            const char* shapeTypeNames[] = { "Box", "Sphere", "Capsule", "Convex Hull", "Mesh" };
            int shapeType = static_cast<int>(collisionShape->getType());
            if (shapeType >= 0 && shapeType < 5) {
                ImGui::Text("Shape Type: %s", shapeTypeNames[shapeType]);
            } else {
                ImGui::Text("Shape Type: Unknown");
            }

            // Shape-specific properties
            if (auto boxShape = std::dynamic_pointer_cast<physics::collision::BoxShape>(collisionShape)) {
                glm::vec3 halfExtents = boxShape->getHalfExtents();
                ImGui::Text("Half Extents: %.3f, %.3f, %.3f", halfExtents.x, halfExtents.y, halfExtents.z);
                ImGui::Text("Full Size: %.3f, %.3f, %.3f", halfExtents.x * 2, halfExtents.y * 2, halfExtents.z * 2);
            }
            else if (auto sphereShape = std::dynamic_pointer_cast<physics::collision::SphereShape>(collisionShape)) {
                float radius = sphereShape->getRadius();
                ImGui::Text("Radius: %.3f", radius);
                ImGui::Text("Diameter: %.3f", radius * 2);
            }

            if (ImGui::Button("Remove Shape", ImVec2(120, 25))) {
                component->setCollisionShape(nullptr);
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No collision shape assigned");

            ImGui::Text("Create Shape:");

            // Box Shape
            static glm::vec3 boxHalfExtents{0.5f, 0.5f, 0.5f};
            ImGui::Text("Box Half Extents:");
            renderVec3Control("Box Size", boxHalfExtents, 0.5f);
            if (ImGui::Button("Create Box Shape", ImVec2(150, 25))) {
                component->createBoxShape(boxHalfExtents);
            }

            // Sphere Shape
            static float sphereRadius = 0.5f;
            ImGui::DragFloat("Sphere Radius", &sphereRadius, 0.01f, 0.01f, 10.0f, "%.3f");
            if (ImGui::Button("Create Sphere Shape", ImVec2(150, 25))) {
                component->createSphereShape(sphereRadius);
            }

            // TODO: Implement capsule shape creation
            // Capsule Shape
            static float capsuleRadius = 0.5f;
            static float capsuleHeight = 2.0f;
            ImGui::DragFloat("Capsule Radius", &capsuleRadius, 0.01f, 0.01f, 10.0f, "%.3f");
            ImGui::DragFloat("Capsule Height", &capsuleHeight, 0.01f, 0.01f, 10.0f, "%.3f");
            if (ImGui::Button("Create Capsule Shape", ImVec2(150, 25))) {
                // component->createCapsuleShape(capsuleRadius, capsuleHeight);
                OHAO_LOG_WARNING("Capsule shape not yet implemented");
            }
        }
    }

    // === Debug Information ===
    if (ImGui::CollapsingHeader("Debug Info")) {
        auto rigidBody = component->getRigidBody();
        if (rigidBody) {
            ImGui::Text("RigidBody: %p", (void*)rigidBody.get());
            ImGui::Text("Position: %.2f, %.2f, %.2f",
                       rigidBody->getPosition().x,
                       rigidBody->getPosition().y,
                       rigidBody->getPosition().z);
            ImGui::Text("Mass: %.2f kg", rigidBody->getMass());
            ImGui::Text("Awake: %s", rigidBody->isAwake() ? "Yes" : "No");

            // Force info
            glm::vec3 accumulatedForce = rigidBody->getAccumulatedForce();
            ImGui::Text("Accumulated Force: %.2f, %.2f, %.2f",
                       accumulatedForce.x, accumulatedForce.y, accumulatedForce.z);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No RigidBody instance");
        }

        auto physicsWorld = component->getPhysicsWorld();
        ImGui::Text("Physics World: %p", (void*)physicsWorld);

        auto transformComponent = component->getTransformComponent();
        ImGui::Text("Transform Component: %p", (void*)transformComponent);

        if (transformComponent) {
            glm::vec3 pos = transformComponent->getPosition();
            ImGui::Text("Transform Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
        }
    }
}

bool PhysicsComponentPanel::renderVec3Control(const std::string& label, glm::vec3& values, float resetValue) {
    bool changed = false;
    ImGui::PushID(label.c_str());

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::Text("%s", label.c_str());
    ImGui::NextColumn();

    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

    float lineHeight = ImGui::GetFontSize() + GImGui->Style.FramePadding.y * 2.0f;
    ImVec2 buttonSize = { lineHeight + 3.0f, lineHeight };

    // X component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.9f, 0.2f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.8f, 0.1f, 0.15f, 1.0f});
    if (ImGui::Button("X", buttonSize)) {
        values.x = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##X", &values.x, 0.1f)) changed = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Y component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.3f, 0.8f, 0.3f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.2f, 0.7f, 0.2f, 1.0f});
    if (ImGui::Button("Y", buttonSize)) {
        values.y = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##Y", &values.y, 0.1f)) changed = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();

    // Z component
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.2f, 0.35f, 0.9f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.1f, 0.25f, 0.8f, 1.0f});
    if (ImGui::Button("Z", buttonSize)) {
        values.z = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::DragFloat("##Z", &values.z, 0.1f)) changed = true;
    ImGui::PopItemWidth();

    ImGui::Columns(1);
    ImGui::PopID();

    return changed;
}

} // namespace ohao
