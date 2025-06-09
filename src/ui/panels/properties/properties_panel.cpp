#include "properties_panel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "scene/scene_node.hpp"
#include "console_widget.hpp"
#include "vulkan_context.hpp"


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

    // SceneObject-specific components
    if (auto actor = asActor(node)) {
        // If this is an Actor, use the Actor-specific component rendering
        renderActorProperties(actor);
    } else if (auto obj = asSceneObject(node)) {
        // Material component for legacy scene objects
        if (ImGui::CollapsingHeader("Material")) {
            renderMaterialProperties(obj);
        }
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
            
            // Wireframe toggle
            if (auto* vulkanContext = VulkanContext::getContextInstance()) {
                bool wireframe = vulkanContext->isWireframeMode();
                if (ImGui::Checkbox("Wireframe Mode", &wireframe)) {
                    vulkanContext->setWireframeMode(wireframe);
                }
            }
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

    float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
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

void PropertiesPanel::renderMaterialProperties(SceneObject* object) {
    if (!object) return;

    ImGui::Separator();
    
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
    Material& material = object->getMaterial();
        bool changed = false;

        // Base color picker
        float color[3] = {
            material.baseColor.r,
            material.baseColor.g,
            material.baseColor.b
        };
        
        if (ImGui::ColorEdit3("Base Color", color)) {
            material.baseColor = glm::vec3(color[0], color[1], color[2]);
            changed = true;
    }

        // PBR properties
    if (ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f)) {
            changed = true;
    }
        
    if (ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f)) {
            changed = true;
    }
        
    if (ImGui::SliderFloat("AO", &material.ao, 0.0f, 1.0f)) {
            changed = true;
    }

        // Emissive color
        float emissive[3] = {
            material.emissive.r,
            material.emissive.g,
            material.emissive.b
        };
        
        if (ImGui::ColorEdit3("Emissive", emissive)) {
            material.emissive = glm::vec3(emissive[0], emissive[1], emissive[2]);
            changed = true;
    }

        // IOR (Index of Refraction)
        if (ImGui::SliderFloat("IOR", &material.ior, 1.0f, 2.5f)) {
            changed = true;
    }

        if (changed && object) {
            // Update material - no need to call scene method since we're modifying it directly
            object->setMaterial(material);
        }
    }
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
    
    // Wireframe toggle
            if (auto* vulkanContext = VulkanContext::getContextInstance()) {
                bool wireframe = vulkanContext->isWireframeMode();
                if (ImGui::Checkbox("Wireframe Mode", &wireframe)) {
                    vulkanContext->setWireframeMode(wireframe);
                }
    }
    
    // Material properties (basic)
    if (model && !model->materials.empty()) {
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& material = model->materials.begin()->second;
            
            // Base color
            glm::vec3 ambient = material.ambient;
            if (ImGui::ColorEdit3("Ambient", glm::value_ptr(ambient))) {
                material.ambient = ambient;
            }
            
            glm::vec3 diffuse = material.diffuse;
            if (ImGui::ColorEdit3("Diffuse", glm::value_ptr(diffuse))) {
                material.diffuse = diffuse;
            }
            
            glm::vec3 specular = material.specular;
            if (ImGui::ColorEdit3("Specular", glm::value_ptr(specular))) {
                material.specular = specular;
            }
            
            float shininess = material.shininess;
            if (ImGui::SliderFloat("Shininess", &shininess, 1.0f, 128.0f)) {
                material.shininess = shininess;
            }
        }
    }
}

void PropertiesPanel::renderPhysicsComponentProperties(PhysicsComponent* component) {
    if (!component) return;
    
    // For now, just display basic info since we don't know the actual PhysicsComponent API
    ImGui::Text("Physics Component: %p", (void*)component);
    ImGui::TextDisabled("Full physics component editor is coming soon!");
    
    // Basic properties that would be common for most physics systems
    float density = 1.0f;  // Default value
    if (ImGui::SliderFloat("Density", &density, 0.1f, 10.0f)) {
        // component->setDensity(density);
    }
    
    bool isStatic = false;  // Default value
    if (ImGui::Checkbox("Static Object", &isStatic)) {
        // component->setStatic(isStatic);
    }
    
    float friction = 0.5f;  // Default value
    if (ImGui::SliderFloat("Friction", &friction, 0.0f, 1.0f)) {
        // component->setFriction(friction);
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

} // namespace ohao
