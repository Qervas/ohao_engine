#include "mesh_component.hpp"
#include "../actor/actor.hpp"
#include "../asset/model.hpp"
#include "../material/material.hpp"
#include "../scene/scene.hpp"
#include "../../renderer/vulkan_context.hpp"
#include "../../ui/components/console_widget.hpp"

namespace ohao {

MeshComponent::MeshComponent(Actor* owner)
    : Component(owner)
    , material(nullptr)
    , ownsMaterial(false)
    , castShadows(true)
    , receiveShadows(true)
    , visible(true)
{
    // Create a default material
    material = new Material();
    ownsMaterial = true;
}

MeshComponent::~MeshComponent() {
    if (material && ownsMaterial) {
        delete material;
        material = nullptr;
    }
}

void MeshComponent::setModel(std::shared_ptr<Model> newModel) {
    if (model == newModel) return;
    
    beginModification();
    model = newModel;
    onModelChanged();
    endModification();
}

std::shared_ptr<Model> MeshComponent::getModel() const {
    return model;
}

void MeshComponent::setMaterial(const Material& newMaterial) {
    beginModification();
    
    // Delete old material if owned
    if (material && ownsMaterial) {
        delete material;
    }
    
    // Create a copy of the material
    material = new Material(newMaterial);
    ownsMaterial = true;
    
    onMaterialChanged();
    endModification();
}

Material& MeshComponent::getMaterial() {
    if (!material) {
        material = new Material();
        ownsMaterial = true;
    }
    return *material;
}

const Material& MeshComponent::getMaterial() const {
    static Material defaultMaterial;
    return material ? *material : defaultMaterial;
}

void MeshComponent::setCastShadows(bool shouldCastShadows) {
    if (castShadows == shouldCastShadows) return;
    
    beginModification();
    castShadows = shouldCastShadows;
    endModification();
}

bool MeshComponent::getCastShadows() const {
    return castShadows;
}

void MeshComponent::setReceiveShadows(bool shouldReceiveShadows) {
    if (receiveShadows == shouldReceiveShadows) return;
    
    beginModification();
    receiveShadows = shouldReceiveShadows;
    endModification();
}

bool MeshComponent::getReceiveShadows() const {
    return receiveShadows;
}

void MeshComponent::setVisible(bool isVisible) {
    if (visible == isVisible) return;
    
    beginModification();
    visible = isVisible;
    endModification();
}

bool MeshComponent::isVisible() const {
    return visible;
}

void MeshComponent::initialize() {
    // Initialize any resources needed
}

void MeshComponent::render() {
    if (!visible || !model) return;
    
    // The actual rendering happens in the renderer system
}

void MeshComponent::destroy() {
    // Clean up any resources
    model.reset();
    
    if (material && ownsMaterial) {
        delete material;
        material = nullptr;
        ownsMaterial = false;
    }
}

const char* MeshComponent::getTypeName() const {
    return "MeshComponent";
}

void MeshComponent::onModelChanged() {
    // Notify scene that the mesh component has been modified
    auto owner = getOwner();
    if (!owner) {
        // Log warning about missing owner
        std::cerr << "MeshComponent::onModelChanged - No owner set" << std::endl;
        return;
    }
    
    auto scene = owner->getScene();
    if (scene) {
        OHAO_LOG_DEBUG("Mesh component changed for actor: " + owner->getName());
        scene->onMeshComponentChanged(this);
        
        // Ensure this component is in the scene's mesh components list
        scene->onMeshComponentAdded(this);
        
        // Mark scene as needing update
        scene->setDirty();
        
        // Force a buffer update in the VulkanContext if available
        if (auto context = VulkanContext::getContextInstance()) {
            context->updateSceneBuffers();
        }
    } else {
        // Log warning about missing scene
        std::cerr << "MeshComponent::onModelChanged - Owner has no scene set" << std::endl;
    }
}

void MeshComponent::onMaterialChanged() {
    // Notify scene that the mesh component has been modified
    if (auto scene = getScene()) {
        scene->onMeshComponentChanged(this);
        }
    }

nlohmann::json MeshComponent::serialize() const {
    nlohmann::json data;
    
    // Store mesh properties
    data["visible"] = visible;
    data["castShadows"] = castShadows;
    data["receiveShadows"] = receiveShadows;
    
    // Store material if it exists
    if (material) {
        // TODO: Implement proper material serialization
        data["material"] = {}; // Placeholder
    }
    
    // Store model reference if it exists
    if (model) {
        // Store model path or ID for loading
        // TODO: Implement proper model path serialization
        data["model"] = {}; // Placeholder
    }
    
    return data;
}

void MeshComponent::deserialize(const nlohmann::json& data) {
    beginModification();
    
    // Load mesh properties
    if (data.contains("visible")) {
        visible = data["visible"];
    }
    
    if (data.contains("castShadows")) {
        castShadows = data["castShadows"];
    }
    
    if (data.contains("receiveShadows")) {
        receiveShadows = data["receiveShadows"];
    }
    
    // Load material if it exists
    if (data.contains("material")) {
        // TODO: Implement proper material deserialization
    }
    
    // Load model if it exists
    if (data.contains("model")) {
        // TODO: Implement proper model loading
    }
    
    endModification();
}

} // namespace ohao 