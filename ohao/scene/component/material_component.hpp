#pragma once

#include "scene/component/component.hpp"
#include "gpu/vulkan/material.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace ohao {


class MaterialComponent : public Component {
public:
    using Ptr = std::shared_ptr<MaterialComponent>;

    MaterialComponent();
    ~MaterialComponent() override;

    // Material management
    void setMaterial(const Material& material);
    [[nodiscard]] Material& getMaterial();
    [[nodiscard]] const Material& getMaterial() const;

    // Texture management (paths only - actual loading is renderer-specific)
    void setAlbedoTexture(std::string_view path);
    void setNormalTexture(std::string_view path);
    void setMetallicTexture(std::string_view path);
    void setRoughnessTexture(std::string_view path);
    void setAoTexture(std::string_view path);
    void setEmissiveTexture(std::string_view path);

    // Material presets
    void applyPreset(Material::Type type);

    // Texture access
    [[nodiscard]] bool hasTextures() const;
    [[nodiscard]] const std::string& getAlbedoTexture() const;
    [[nodiscard]] const std::string& getNormalTexture() const;
    [[nodiscard]] const std::string& getMetallicTexture() const;
    [[nodiscard]] const std::string& getRoughnessTexture() const;
    [[nodiscard]] const std::string& getAoTexture() const;
    [[nodiscard]] const std::string& getEmissiveTexture() const;

    // Component overrides
    [[nodiscard]] const char* getTypeName() const override;
    void initialize() override;
    void render() override;
    void destroy() override;

private:
    Material material;

    // Helper to update texture usage flags
    void updateTextureUsage();
};

} // namespace ohao
