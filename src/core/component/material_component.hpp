#pragma once

#include "component.hpp"
#include "../material/material.hpp"
#include "../../renderer/texture/texture_manager.hpp"
#include <memory>

namespace ohao {

class MaterialComponent : public Component {
public:
    using Ptr = std::shared_ptr<MaterialComponent>;
    
    MaterialComponent();
    ~MaterialComponent() override;
    
    // Material management
    void setMaterial(const Material& material);
    Material& getMaterial();
    const Material& getMaterial() const;
    
    // Texture management
    void setAlbedoTexture(const std::string& path);
    void setNormalTexture(const std::string& path);
    void setMetallicTexture(const std::string& path);
    void setRoughnessTexture(const std::string& path);
    void setAoTexture(const std::string& path);
    void setEmissiveTexture(const std::string& path);
    
    // Material presets
    void applyPreset(Material::Type type);
    
    // Texture access
    bool hasTextures() const;
    const std::string& getAlbedoTexture() const;
    const std::string& getNormalTexture() const;
    const std::string& getMetallicTexture() const;
    const std::string& getRoughnessTexture() const;
    const std::string& getAoTexture() const;
    const std::string& getEmissiveTexture() const;
    
    // Component overrides
    const char* getTypeName() const override;
    void initialize() override;
    void render() override;
    void destroy() override;
    
    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
private:
    Material material;
    
    // Texture manager reference (set during initialization)
    TextureManager* textureManager{nullptr};
    
    // Helper to update texture usage flags
    void updateTextureUsage();
};

} // namespace ohao