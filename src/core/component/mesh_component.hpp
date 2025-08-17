#pragma once

#include "component.hpp"
#include "../asset/model.hpp"
#include <memory>

namespace ohao {

class MeshComponent : public Component {
public:
    using Ptr = std::shared_ptr<MeshComponent>;
    
    MeshComponent();
    ~MeshComponent() override;
    
    // Model/Geometry management
    void setModel(std::shared_ptr<Model> model);
    std::shared_ptr<Model> getModel() const;
    
    // Visibility
    void setVisible(bool visible);
    bool isVisible() const;
    
    // Rendering mode (affects how geometry is rendered)
    enum class RenderMode {
        SOLID,
        WIREFRAME,
        POINTS
    };
    
    void setRenderMode(RenderMode mode);
    RenderMode getRenderMode() const;
    
    // Geometry properties
    uint32_t getVertexCount() const;
    uint32_t getIndexCount() const;
    
    // Component overrides
    const char* getTypeName() const override;
    void initialize() override;
    void render() override;
    void destroy() override;
    
    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
    // Buffer management (used by renderer)
    void setBufferOffsets(uint32_t vertexOffset, uint32_t indexOffset, uint32_t indexCount);
    uint32_t getVertexOffset() const { return vertexOffset; }
    uint32_t getIndexOffset() const { return indexOffset; }
    uint32_t getBufferIndexCount() const { return indexCount; }
    
private:
    std::shared_ptr<Model> model;
    bool visible;
    RenderMode renderMode;
    
    // Cached buffer data - specific to renderer implementation
    uint32_t vertexOffset{0};  // Offset in combined buffer
    uint32_t indexOffset{0};   // Offset in combined buffer
    uint32_t indexCount{0};    // Number of indices for this mesh
};

} // namespace ohao 