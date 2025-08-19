#include "properties_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "scene/scene_node.hpp"
#include "console_widget.hpp"
#include "vulkan_context.hpp"
#include "core/component/light_component.hpp"
#include "core/component/material_component.hpp"
#include "core/material/material.hpp"
#include "core/physics/dynamics/rigid_body.hpp"
#include "core/physics/collision/shapes/collision_shape.hpp"
#include "core/physics/collision/shapes/box_shape.hpp"
#include "core/physics/collision/shapes/sphere_shape.hpp"
#include <cstring>


namespace ohao {

PropertiesPanel::PropertiesPanel() : PanelBase("Properties") {
    windowFlags = ImGuiWindowFlags_NoCollapse;
}

void PropertiesPanel::render() {
    if (!visible) return;

    ImGui::Begin(name.c_str(), &visible, windowFlags);

    auto* selectedObject = SelectionManager::get().getSelectedObject();
    if (selectedObject) {
        // Check if the object is an Actor first (new system)
        if (auto actor = asActor(selectedObject)) {
            renderActorProperties(actor);
        } else {
            // Fall back to the old system
        renderNodeProperties(selectedObject);
        }
    } else {
        ImGui::TextDisabled("No object selected");
    }

    ImGui::End();
}

void PropertiesPanel::renderNodeProperties(SceneNode* node) {
    if (!node) return;

    // Node name and type header
    char nameBuf[256];
    strcpy(nameBuf, node->getName().c_str());
    if (ImGui::InputText("##Name", nameBuf, sizeof(nameBuf))) {
        node->setName(nameBuf);
    }

    ImGui::SameLine();
    if (auto actor = asActor(node)) {
        ImGui::TextDisabled("(Actor)");
    } else if (auto obj = asSceneObject(node)) {
        ImGui::TextDisabled("(%s)", obj->getTypeName());
    } else {
        ImGui::TextDisabled("(Node)");
    }

    ImGui::Separator();

    // Transform component (available for all nodes)
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderTransformProperties(node);
    }

    // Modern Actor-Component system only
    if (auto actor = asActor(node)) {
        // Use the modern Actor-Component system
        renderActorProperties(actor);
    } else {
        // Non-Actor nodes (should be rare in modern architecture)
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Basic scene node (no components)");
    }
}

void PropertiesPanel::renderActorProperties(Actor* actor) {
    if (!actor) return;
    
    // Actor name and type header
    char nameBuf[256];
    strcpy(nameBuf, actor->getName().c_str());
    if (ImGui::InputText("##Name", nameBuf, sizeof(nameBuf))) {
        actor->setName(nameBuf);
    }
    
    ImGui::SameLine();
    ImGui::TextDisabled("(Actor)");
    
    ImGui::Separator();
    
    // Transform component
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto transformComponent = actor->getTransform();
        if (transformComponent) {
            renderTransformComponentProperties(transformComponent);
        } else {
            ImGui::TextDisabled("No transform component");
            
            // Add a transform component button
            if (ImGui::Button("Add Transform Component")) {
                actor->addComponent<TransformComponent>();
            }
        }
    }
    
    // Model display (find MeshComponent if exists)
    auto meshComponent = actor->getComponent<MeshComponent>();
    if (meshComponent && meshComponent->getModel()) {
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto model = meshComponent->getModel();
            ImGui::Text("Vertices: %zu", model->vertices.size());
            ImGui::Text("Indices: %zu", model->indices.size());
            
            // Material properties accessible through the MeshComponent UI
        }
    }
    
    // Components list and management
    renderComponentProperties(actor);
}

void PropertiesPanel::renderTransformComponentProperties(TransformComponent* transform) {
    if (!transform) return;
    
    bool transformChanged = false;
    
    // Get current transform values
    glm::vec3 position = transform->getPosition();
    glm::vec3 rotation = glm::degrees(transform->getRotationEuler());
    glm::vec3 scale = transform->getScale();
    
    // Display ID in debug builds
    ImGui::Text("Transform Component (ID: %p)", (void*)transform);
    
    if (renderVec3Control("Position", position)) {
        transform->setPosition(position);
        transformChanged = true;
    }
    
    if (renderVec3Control("Rotation", rotation)) {
        transform->setRotationEuler(glm::radians(rotation));
        transformChanged = true;
    }
    
    if (renderVec3Control("Scale", scale, 1.0f)) {
        transform->setScale(scale);
        transformChanged = true;
    }
    
    // Display world transform info
    if (ImGui::TreeNode("World Transform")) {
        glm::vec3 worldPos = transform->getWorldPosition();
        glm::vec3 worldRot = glm::degrees(transform->getRotationEuler());
        glm::vec3 worldScale = transform->getWorldScale();
        
        ImGui::Text("World Position: %.2f, %.2f, %.2f", worldPos.x, worldPos.y, worldPos.z);
        ImGui::Text("World Rotation: %.2f, %.2f, %.2f", worldRot.x, worldRot.y, worldRot.z);
        ImGui::Text("World Scale: %.2f, %.2f, %.2f", worldScale.x, worldScale.y, worldScale.z);
        
        ImGui::TreePop();
    }
    
    // If transform changed, update scene buffers
    if (transformChanged && VulkanContext::getContextInstance()) {
        VulkanContext::getContextInstance()->updateSceneBuffers();
    }
}

void PropertiesPanel::renderTransformProperties(SceneNode* node) {
    if (!node) return;
    
    // Check if node is actually an Actor (new system)
    Actor* actor = dynamic_cast<Actor*>(node);
    if (actor) {
        // Use the TransformComponent from the Actor
        auto transformComponent = actor->getTransform();
        if (!transformComponent) {
            ImGui::TextDisabled("No transform component found");
            return;
        }
        
        bool transformChanged = false;
        
        // Get current transform values
        glm::vec3 position = transformComponent->getPosition();
        glm::vec3 rotation = glm::degrees(transformComponent->getRotationEuler());
        glm::vec3 scale = transformComponent->getScale();
        
        // Show object info in debug builds
        ImGui::Text("Object: %s (ID: %zu)", actor->getName().c_str(), actor->getID());
        
        if (renderVec3Control("Position", position)) {
            transformComponent->setPosition(position);
            transformChanged = true;
        }
        
        if (renderVec3Control("Rotation", rotation)) {
            transformComponent->setRotationEuler(glm::radians(rotation));
            transformChanged = true;
        }
        
        if (renderVec3Control("Scale", scale, 1.0f)) {
            transformComponent->setScale(scale);
            transformChanged = true;
        }
        
        // Display world transform info
        if (ImGui::TreeNode("World Transform")) {
            glm::vec3 worldPos = transformComponent->getWorldPosition();
            glm::vec3 worldRot = glm::degrees(transformComponent->getRotationEuler());
            glm::vec3 worldScale = transformComponent->getWorldScale();
            
            ImGui::Text("World Position: %.2f, %.2f, %.2f", worldPos.x, worldPos.y, worldPos.z);
            ImGui::Text("World Rotation: %.2f, %.2f, %.2f", worldRot.x, worldRot.y, worldRot.z);
            ImGui::Text("World Scale: %.2f, %.2f, %.2f", worldScale.x, worldScale.y, worldScale.z);
            
            ImGui::TreePop();
        }
        
        // If transform changed, update scene buffers
        if (transformChanged && VulkanContext::getContextInstance()) {
            VulkanContext::getContextInstance()->updateSceneBuffers();
        }
        
        return;
    }
    
    // Fall back to old system for backward compatibility
    SceneObject* sceneObject = asSceneObject(node);
    if (!sceneObject) {
        ImGui::TextDisabled("Transform properties only available for SceneObjects");
        return;
    }

    // Directly access the transform from the node to ensure we're modifying the right object
    Transform& transform = node->getTransform();
    bool transformChanged = false;
    
    // Get current transform values
    glm::vec3 position = transform.getLocalPosition();
    glm::vec3 rotation = glm::degrees(glm::eulerAngles(transform.getLocalRotation()));
    glm::vec3 scale = transform.getLocalScale();

    // Show transform ID in debug builds to help troubleshoot
    ImGui::Text("Object: %s (addr: %p)", node->getName().c_str(), (void*)node);

    if (renderVec3Control("Position", position)) {
        transform.setLocalPosition(position);
        transformChanged = true;
    }

    if (renderVec3Control("Rotation", rotation)) {
        transform.setLocalRotationEuler(glm::radians(rotation));
        transformChanged = true;
    }

    if (renderVec3Control("Scale", scale, 1.0f)) {
        transform.setLocalScale(scale);
        transformChanged = true;
    }

    // If transform changed, update scene buffers
    if (transformChanged) {
        // Mark the transform as dirty - this is crucial
        node->markTransformDirty();
        
        if (auto context = VulkanContext::getContextInstance()) {
            // Explicitly update scene buffers
            context->updateSceneBuffers();
        }
    }

    // Display world transform info
    if (ImGui::TreeNode("World Transform")) {
        glm::vec3 worldPos = transform.getWorldPosition();
        glm::vec3 worldRot = glm::degrees(glm::eulerAngles(transform.getWorldRotation()));
        glm::vec3 worldScale = transform.getWorldScale();

        ImGui::Text("World Position: %.2f, %.2f, %.2f", worldPos.x, worldPos.y, worldPos.z);
        ImGui::Text("World Rotation: %.2f, %.2f, %.2f", worldRot.x, worldRot.y, worldRot.z);
        ImGui::Text("World Scale: %.2f, %.2f, %.2f", worldScale.x, worldScale.y, worldScale.z);

        ImGui::TreePop();
    }
}

bool PropertiesPanel::renderVec3Control(const std::string& label, glm::vec3& values, float resetValue) {
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

void PropertiesPanel::renderComponentProperties(Actor* actor) {
    if (!actor) return;
    
    if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen)) {
        // List all components with an option to remove them
        const auto& components = actor->getAllComponents();
        if (components.empty()) {
            ImGui::TextDisabled("No components attached");
        } else {
            // Create a table for better layout
            if (ImGui::BeginTable("ComponentsTable", 2, ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();
                
                // Track components to remove by index instead of pointer
                std::vector<size_t> indicesToRemove;
                
                for (size_t i = 0; i < components.size(); i++) {
                    auto& component = components[i];
                    
                    // Skip transform component as it's managed separately
                    if (component.get() == actor->getTransform()) {
                        continue;
        }
                    
                    ImGui::TableNextRow();
                    
                    // Component name and type
                    ImGui::TableSetColumnIndex(0);
                    // Create a unique ID using string + int
                    std::string nodeLabel = std::string(component->getTypeName()) + "##" + std::to_string(i);
                    bool isOpen = ImGui::TreeNodeEx(nodeLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
                    
                    // Removal button
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Button("Remove")) {
                        // Queue the index for removal
                        indicesToRemove.push_back(i);
                    }
                    ImGui::PopID();
                    
                    // Component-specific properties
                    if (isOpen) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Columns(1);
                        
                        // Render component-specific properties
                        if (auto transformComponent = dynamic_cast<TransformComponent*>(component.get())) {
                            // Transform is already displayed separately
                            ImGui::TextDisabled("Transform properties are shown in the Transform section");
                        } 
                        else if (auto meshComponent = dynamic_cast<MeshComponent*>(component.get())) {
                            // Mesh properties
                            renderMeshComponentProperties(meshComponent);
                        }
                        else if (auto physicsComponent = dynamic_cast<PhysicsComponent*>(component.get())) {
                            // Physics properties
                            renderPhysicsComponentProperties(physicsComponent);
                        }
                        else if (auto lightComponent = dynamic_cast<LightComponent*>(component.get())) {
                            // Light properties
                            renderLightComponentProperties(lightComponent);
                        }
                        else if (auto materialComponent = dynamic_cast<MaterialComponent*>(component.get())) {
                            // Material properties
                            renderMaterialComponentProperties(materialComponent);
                        }
                        else {
                            // Generic component properties
                            ImGui::TextDisabled("No editable properties available for this component");
                        }
                        
                        ImGui::TreePop();
                    }
                }
                
                ImGui::EndTable();

                // Process removals (in reverse order to avoid invalidating indices)
                std::sort(indicesToRemove.begin(), indicesToRemove.end(), std::greater<size_t>());
                for (auto index : indicesToRemove) {
                    // We can't directly call removeComponent with the component pointer
                    // Instead, we'll check the type and use the appropriate template method
                    
                    auto& component = components[index];
                    
                    if (dynamic_cast<MeshComponent*>(component.get())) {
                        actor->removeComponent<MeshComponent>();
                    }
                    else if (dynamic_cast<PhysicsComponent*>(component.get())) {
                        actor->removeComponent<PhysicsComponent>();
                    }
                    // Add more component types here as needed
                }
            }
        }
        
        // Add Component button and dropdown
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("AddComponentPopup");
        }
        
        if (ImGui::BeginPopup("AddComponentPopup")) {
            ImGui::TextUnformatted("Component Types");
            ImGui::Separator();
            
            if (ImGui::MenuItem("Transform Component")) {
                if (actor->getComponent<TransformComponent>() == nullptr) {
                    actor->addComponent<TransformComponent>();
        } else {
                    ImGui::CloseCurrentPopup();
                    ImGui::OpenPopup("TransformAlreadyExistsPopup");
                }
            }
            
            if (ImGui::MenuItem("Mesh Component")) {
                actor->addComponent<MeshComponent>();
            }
            
            if (ImGui::MenuItem("Physics Component")) {
                try {
                    actor->addComponent<PhysicsComponent>();
                } catch (const std::exception& e) {
                    ImGui::CloseCurrentPopup();
                    errorMessage = "Failed to add Physics component: " + std::string(e.what());
                    showErrorPopup = true;
                }
            }
            
            ImGui::EndPopup();
        }
        
        // Error popup
        if (showErrorPopup) {
            ImGui::OpenPopup("ComponentErrorPopup");
            showErrorPopup = false;
        }
        
        if (ImGui::BeginPopupModal("ComponentErrorPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", errorMessage.c_str());
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
                errorMessage.clear();
            }
            ImGui::EndPopup();
        }
        
        // Transform already exists popup
        if (ImGui::BeginPopupModal("TransformAlreadyExistsPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("An actor can only have one Transform component.");
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void PropertiesPanel::renderMeshComponentProperties(MeshComponent* component) {
    if (!component) return;
    
    ImGui::Text("Mesh Component Properties");
    ImGui::Separator();
    
    // Show model information if available
    auto model = component->getModel();
    if (model) {
        ImGui::Text("Model Information:");
            ImGui::Text("Vertices: %zu", model->vertices.size());
            ImGui::Text("Indices: %zu", model->indices.size());
            ImGui::Text("Materials: %zu", model->materials.size());

        // Option to replace the model
        if (ImGui::Button("Replace Model")) {
            ImGui::OpenPopup("ReplaceModelPopup");
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No model assigned");
        
        // Button to add a model
        if (ImGui::Button("Add Model")) {
            ImGui::OpenPopup("AddModelPopup");
        }
    }
    
    // Model selection popup
    if (ImGui::BeginPopup("AddModelPopup") || ImGui::BeginPopup("ReplaceModelPopup")) {
        ImGui::Text("Select Primitive Type:");
        
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
        
        if (ImGui::Selectable("Plane")) {
            auto planeModel = generatePrimitiveMesh(PrimitiveType::Plane);
            component->setModel(planeModel);
            ImGui::CloseCurrentPopup();
            
            // Update scene buffers
            if (auto context = VulkanContext::getContextInstance()) {
                context->updateSceneBuffers();
            }
        }
        
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
}

void PropertiesPanel::renderPhysicsComponentProperties(PhysicsComponent* component) {
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
            // TODO: Implement CapsuleShape and ConvexHullShape
            /*
            else if (auto capsuleShape = std::dynamic_pointer_cast<CapsuleShape>(collisionShape)) {
                float radius = capsuleShape->getRadius();
                float height = capsuleShape->getHeight();
                ImGui::Text("Radius: %.3f", radius);
                ImGui::Text("Height: %.3f", height);
            }
            else if (auto hullShape = std::dynamic_pointer_cast<ConvexHullShape>(collisionShape)) {
                ImGui::Text("Vertices: %zu", hullShape->getPoints().size());
            }
            */
            
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

std::shared_ptr<Model> PropertiesPanel::generatePrimitiveMesh(PrimitiveType type) {
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
            OHAO_LOG("Cube model created");
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
            OHAO_LOG("Sphere model created");
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
            OHAO_LOG("Plane model created");
            break;
        }

        case PrimitiveType::Empty:
        default:
            // Empty object has no geometry
            OHAO_LOG("Empty model created");
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

void PropertiesPanel::renderLightComponentProperties(LightComponent* component) {
    if (!component) return;
    
    ImGui::Text("Light Component Properties");
    ImGui::Separator();
    
    // Light Type Selection
    const char* lightTypeNames[] = { "Directional", "Point", "Spot" };
    int currentType = static_cast<int>(component->getLightType());
    if (ImGui::Combo("Light Type", &currentType, lightTypeNames, 3)) {
        component->setLightType(static_cast<LightType>(currentType));
    }
    
    // Color Control
    glm::vec3 color = component->getColor();
    if (ImGui::ColorEdit3("Color", glm::value_ptr(color))) {
        component->setColor(color);
    }
    
    // Intensity Control
    float intensity = component->getIntensity();
    if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 10.0f)) {
        component->setIntensity(intensity);
    }
    
    // Type-specific properties
    LightType lightType = component->getLightType();
    
    if (lightType == LightType::Point || lightType == LightType::Spot) {
        // Range control for point and spot lights
        float range = component->getRange();
        if (ImGui::SliderFloat("Range", &range, 1.0f, 100.0f)) {
            component->setRange(range);
        }
    }
    
    if (lightType == LightType::Directional || lightType == LightType::Spot) {
        // Direction control for directional and spot lights
        glm::vec3 direction = component->getDirection();
        if (renderVec3Control("Direction", direction, 0.0f)) {
            // Normalize the direction vector
            if (glm::length(direction) > 0.0f) {
                direction = glm::normalize(direction);
            } else {
                direction = glm::vec3(0.0f, -1.0f, 0.0f); // Default down direction
            }
            component->setDirection(direction);
        }
    }
    
    if (lightType == LightType::Spot) {
        // Cone angle controls for spot lights
        float innerCone = component->getInnerConeAngle();
        float outerCone = component->getOuterConeAngle();
        
        if (ImGui::SliderFloat("Inner Cone Angle", &innerCone, 1.0f, 89.0f)) {
            // Ensure inner cone is smaller than outer cone
            if (innerCone >= outerCone) {
                innerCone = outerCone - 1.0f;
            }
            component->setInnerConeAngle(innerCone);
        }
        
        if (ImGui::SliderFloat("Outer Cone Angle", &outerCone, 2.0f, 90.0f)) {
            // Ensure outer cone is larger than inner cone
            if (outerCone <= innerCone) {
                outerCone = innerCone + 1.0f;
            }
            component->setOuterConeAngle(outerCone);
        }
    }
    
    // Light information display
    ImGui::Separator();
    ImGui::Text("Light Information:");
    ImGui::Text("Type: %s", lightTypeNames[currentType]);
    ImGui::Text("Intensity: %.2f", component->getIntensity());
    if (lightType == LightType::Point || lightType == LightType::Spot) {
        ImGui::Text("Range: %.2f", component->getRange());
    }
    if (lightType == LightType::Directional || lightType == LightType::Spot) {
        glm::vec3 dir = component->getDirection();
        ImGui::Text("Direction: (%.2f, %.2f, %.2f)", dir.x, dir.y, dir.z);
    }
    if (lightType == LightType::Spot) {
        ImGui::Text("Inner Cone: %.1f°", component->getInnerConeAngle());
        ImGui::Text("Outer Cone: %.1f°", component->getOuterConeAngle());
    }
}

void PropertiesPanel::renderMaterialComponentProperties(MaterialComponent* component) {
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
    strcpy_s(nameBuffer, material.name.c_str());
    if (ImGui::InputText("Material Name##material_name", nameBuffer, sizeof(nameBuffer))) {
        material.name = std::string(nameBuffer);
    }
    
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

void PropertiesPanel::renderPBRMaterialProperties(Material& material) {
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
