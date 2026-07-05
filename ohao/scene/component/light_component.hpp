#pragma once
#include "scene/component/component.hpp"
#include <glm/glm.hpp>

namespace ohao {

enum class LightType {
    Sphere = 0,       // point light with radius (soft shadows)
    Directional = 1,  // sun/moon (parallel rays)
    Spot = 2,         // cone light
    AreaRect = 3,     // rectangular area light
};

class LightComponent : public Component {
public:
    LightComponent();
    ~LightComponent() override = default;

    // Component interface
    void initialize() override;
    void update(float deltaTime) override;
    const char* getTypeName() const override { return "LightComponent"; }

    // Light properties
    void setLightType(LightType type) { lightType = type; }
    LightType getLightType() const { return lightType; }

    void setColor(const glm::vec3& color) { lightColor = color; }
    const glm::vec3& getColor() const { return lightColor; }

    void setIntensity(float intensity) { lightIntensity = intensity; }
    float getIntensity() const { return lightIntensity; }

    void setRange(float range) { lightRange = range; }
    float getRange() const { return lightRange; }

    // For spot lights
    void setInnerConeAngle(float angle) { innerConeAngle = angle; }
    float getInnerConeAngle() const { return innerConeAngle; }

    void setOuterConeAngle(float angle) { outerConeAngle = angle; }
    float getOuterConeAngle() const { return outerConeAngle; }

    // Direction (directional + spot)
    void setDirection(const glm::vec3& direction) { lightDirection = glm::normalize(direction); }
    const glm::vec3& getDirection() const { return lightDirection; }

    // Radius (sphere lights)
    void setRadius(float r) { radius = r; }
    float getRadius() const { return radius; }

    // Area rect lights — two edge vectors define the quad
    void setAreaEdges(const glm::vec3& e1, const glm::vec3& e2) { edge1 = e1; edge2 = e2; }
    const glm::vec3& getEdge1() const { return edge1; }
    const glm::vec3& getEdge2() const { return edge2; }

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