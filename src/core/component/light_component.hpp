#pragma once
#include "component.hpp"
#include <glm/glm.hpp>
#include <string>
#include <nlohmann/json.hpp>

namespace ohao {

// Enum for different light types
enum class LightType {
    Point,      // Emits light in all directions from a point
    Directional, // Emits light in one direction (like sun)
    Spot,       // Emits light in a cone shape (like flashlight)
    Area        // Emits light from a rectangular surface
};

class LightComponent : public Component {
public:
    LightComponent();
    ~LightComponent() override = default;

    // Component lifecycle methods
    void initialize() override;
    void update(float deltaTime) override;

    // Light properties
    void setColor(const glm::vec3& color);
    void setIntensity(float intensity);
    void setEnabled(bool enabled);
    void setRange(float range);
    void setType(LightType type);
    
    // Directional light properties
    void setDirection(const glm::vec3& direction);
    
    // Spot light properties
    void setInnerConeAngle(float angle);
    void setOuterConeAngle(float angle);
    
    // Area light properties
    void setWidth(float width);
    void setHeight(float height);
    
    // Getters
    const glm::vec3& getColor() const { return color; }
    float getIntensity() const { return intensity; }
    bool isEnabled() const { return enabled; }
    float getRange() const { return range; }
    LightType getType() const { return type; }
    const glm::vec3& getDirection() const { return direction; }
    float getInnerConeAngle() const { return innerConeAngle; }
    float getOuterConeAngle() const { return outerConeAngle; }
    float getWidth() const { return width; }
    float getHeight() const { return height; }

    // Serialization
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;

    // Clone support
    std::shared_ptr<Component> clone() const;

    // Type information
    static constexpr const char* TypeName = "LightComponent";
    const char* getTypeName() const override { return TypeName; }

private:
    glm::vec3 color{1.0f, 1.0f, 1.0f};  // RGB color
    float intensity{1.0f};              // Light brightness
    bool enabled{true};                // Is light active
    float range{10.0f};                // Light range (for point and spot lights)
    LightType type{LightType::Point};  // Type of light
    
    // Directional and spot light properties
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // Default points down
    
    // Spot light properties
    float innerConeAngle{15.0f};     // Inner cone angle in degrees
    float outerConeAngle{45.0f};     // Outer cone angle in degrees
    
    // Area light properties
    float width{1.0f};              // Width of area light
    float height{1.0f};             // Height of area light
};

} // namespace ohao 