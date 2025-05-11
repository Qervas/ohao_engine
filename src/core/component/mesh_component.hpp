#pragma once

#include "component.hpp"
#include "../asset/model.hpp"
#include "../material/material.hpp"
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace ohao {

class Model;
class Material;

class MeshComponent : public Component {
public:
    using Ptr = std::shared_ptr<MeshComponent>;
    
    MeshComponent(Actor* owner = nullptr);
    ~MeshComponent() override;
    
    // Model management
    void setModel(std::shared_ptr<Model> model);
    std::shared_ptr<Model> getModel() const;
    
    // Material management
    void setMaterial(const Material& material);
    Material& getMaterial();
    const Material& getMaterial() const;
    
    // Mesh properties
    void setCastShadows(bool castShadows);
    bool getCastShadows() const;
    
    void setReceiveShadows(bool receiveShadows);
    bool getReceiveShadows() const;
    
    // Visibility
    void setVisible(bool visible);
    bool isVisible() const;
    
    // Rendering mode
    enum class RenderMode {
        SOLID,
        WIREFRAME,
        TRANSPARENT
    };
    
    void setRenderMode(RenderMode mode);
    RenderMode getRenderMode() const;
    
    // Component interface
    void initialize() override;
    void render() override;
    void destroy() override;
    
    // Type information
    const char* getTypeName() const override;
    static const char* staticTypeName() { return "MeshComponent"; }
    
    // Serialization
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    std::shared_ptr<Model> model;
    Material* material;  // We own this material
    bool ownsMaterial;   // Flag indicating if we should delete the material
    
    bool castShadows;
    bool receiveShadows;
    bool visible;
    
    // Helper methods
    void onModelChanged();
    void onMaterialChanged();
    
    // Cached buffers - specific to renderer implementation
    uint32_t vertexOffset;  // Offset in combined buffer
    uint32_t indexOffset;   // Offset in combined buffer
    uint32_t indexCount;    // Number of indices for this mesh
};

} // namespace ohao 