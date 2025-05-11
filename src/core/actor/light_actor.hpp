#pragma once
#include "actor.hpp"
#include "../component/light_component.hpp"
#include <memory>
#include <string>

namespace ohao {

class LightActor : public Actor {
public:
    enum class LightActorType {
        Point,
        Directional,
        Spot,
        Area
    };

    // Create a specific light type
    static std::shared_ptr<LightActor> createLight(
        const std::string& name, 
        LightActorType type = LightActorType::Point
    );

    // Constructor
    LightActor(const std::string& name = "Light");
    ~LightActor() override = default;

    // Light-specific methods
    void setLightType(LightType type);
    LightType getLightType() const;
    
    // Color control
    void setColor(const glm::vec3& color);
    const glm::vec3& getColor() const;
    
    // Intensity control
    void setIntensity(float intensity);
    float getIntensity() const;
    
    // Enable/disable
    void setLightEnabled(bool enabled);
    bool isLightEnabled() const;
    
    // Range control (for point and spot lights)
    void setRange(float range);
    float getRange() const;
    
    // Directional light properties
    void setDirection(const glm::vec3& direction);
    const glm::vec3& getDirection() const;
    
    // Spot light properties
    void setConeAngles(float inner, float outer);
    float getInnerConeAngle() const;
    float getOuterConeAngle() const;
    
    // Area light properties
    void setDimensions(float width, float height);
    float getWidth() const;
    float getHeight() const;
    
    // Helper for direct component access
    LightComponent* getLightComponent() const;
    
    // Type information
    const char* getTypeName() const override { return "LightActor"; }

private:
    // Helper to ensure we have a light component
    void ensureLightComponent();
};

} // namespace ohao 