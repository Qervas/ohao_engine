#pragma once

#include "component.hpp"
#include "../asset/model.hpp"
#include "../material/material.hpp"
#include <memory>

namespace ohao {

class MeshComponent : public Component {
public:
    using Ptr = std::shared_ptr<MeshComponent>;
    
    MeshComponent();
    ~MeshComponent() override;
    
    // Model management
    void setModel(std::shared_ptr<Model> model);
    std::shared_ptr<Model> getModel() const;
    
    // Material management
    void setMaterial(const Material& material);
    Material& getMaterial();
    const Material& getMaterial() const;
    
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
    
    // Component overrides
    const char* getTypeName() const override;
    void initialize() override;
    void render() override;
    void destroy() override;
    
    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
private:
    std::shared_ptr<Model> model;
    Material material;
    bool visible;
    RenderMode renderMode;
    
    // Cached buffers - specific to renderer implementation
    uint32_t vertexOffset;  // Offset in combined buffer
    uint32_t indexOffset;   // Offset in combined buffer
    uint32_t indexCount;    // Number of indices for this mesh
};

} // namespace ohao 