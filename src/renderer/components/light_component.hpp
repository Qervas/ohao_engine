#pragma once
#include "component.hpp"
#include <glm/glm.hpp>

namespace ohao {

enum class LightType {
    Directional = 0,
    Point = 1,
    Spot = 2
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

    // For directional lights
    void setDirection(const glm::vec3& direction) { lightDirection = glm::normalize(direction); }
    const glm::vec3& getDirection() const { return lightDirection; }

    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;

private:
    LightType lightType;
    glm::vec3 lightColor;
    float lightIntensity;
    float lightRange;
    
    // Spot light properties
    float innerConeAngle;
    float outerConeAngle;
    
    // Directional light properties
    glm::vec3 lightDirection;
};

} // namespace ohao