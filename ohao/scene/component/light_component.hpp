#pragma once

#include "core/concepts.hpp"
#include "scene/component/component.hpp"

#include <glm/glm.hpp>

namespace ohao {

enum class LightType : int {
    Sphere = 0,       // point light with radius (soft shadows)
    Directional = 1,  // sun/moon (parallel rays)
    Spot = 2,         // cone light
    AreaRect = 3,     // rectangular area light
};

class LightComponent : public Component {
public:
    LightComponent();
    ~LightComponent() override = default;

    void initialize() override;
    void update(float deltaTime) override;
    [[nodiscard]] const char* getTypeName() const override { return "LightComponent"; }

    void setLightType(LightType type) noexcept { lightType = type; }
    [[nodiscard]] LightType getLightType() const noexcept { return lightType; }
    [[nodiscard]] int getLightTypeIndex() const noexcept {
        return static_cast<int>(to_underlying(lightType));
    }

    void setColor(const glm::vec3& color) noexcept { lightColor = color; }
    [[nodiscard]] const glm::vec3& getColor() const noexcept { return lightColor; }

    void setIntensity(float intensity) noexcept { lightIntensity = intensity; }
    [[nodiscard]] float getIntensity() const noexcept { return lightIntensity; }

    void setRange(float range) noexcept { lightRange = range; }
    [[nodiscard]] float getRange() const noexcept { return lightRange; }

    void setInnerConeAngle(float angle) noexcept { innerConeAngle = angle; }
    [[nodiscard]] float getInnerConeAngle() const noexcept { return innerConeAngle; }

    void setOuterConeAngle(float angle) noexcept { outerConeAngle = angle; }
    [[nodiscard]] float getOuterConeAngle() const noexcept { return outerConeAngle; }

    void setDirection(const glm::vec3& direction) {
        lightDirection = glm::normalize(direction);
    }
    [[nodiscard]] const glm::vec3& getDirection() const noexcept { return lightDirection; }

    void setRadius(float r) noexcept { radius = r; }
    [[nodiscard]] float getRadius() const noexcept { return radius; }

    void setAreaEdges(const glm::vec3& e1, const glm::vec3& e2) noexcept {
        edge1 = e1;
        edge2 = e2;
    }
    [[nodiscard]] const glm::vec3& getEdge1() const noexcept { return edge1; }
    [[nodiscard]] const glm::vec3& getEdge2() const noexcept { return edge2; }

    /// Designated-style presets
    static void applyDirectionalSun(LightComponent& light,
                                    const glm::vec3& direction = glm::vec3(-0.3f, -1.0f, -0.2f),
                                    float intensity = 3.0f) {
        light.setLightType(LightType::Directional);
        light.setDirection(direction);
        light.setIntensity(intensity);
        light.setColor(glm::vec3(1.0f, 0.98f, 0.95f));
    }

    static void applySphereKey(LightComponent& light,
                               float intensity = 10.0f,
                               float radius = 0.5f) {
        light.setLightType(LightType::Sphere);
        light.setIntensity(intensity);
        light.setRadius(radius);
        light.setColor(glm::vec3(1.0f));
    }

private:
    LightType lightType = LightType::Sphere;
    glm::vec3 lightColor{1.0f};
    float lightIntensity = 10.0f;
    float lightRange = 50.0f;
    float radius = 0.5f;
    float innerConeAngle = 15.0f;
    float outerConeAngle = 30.0f;
    glm::vec3 lightDirection{0.0f, -1.0f, 0.0f};
    glm::vec3 edge1{1.0f, 0.0f, 0.0f};
    glm::vec3 edge2{0.0f, 0.0f, 1.0f};
};

} // namespace ohao
