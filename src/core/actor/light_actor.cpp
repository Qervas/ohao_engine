#include "light_actor.hpp"
#include <glm/gtc/constants.hpp>

namespace ohao {

// Static factory method to create different light types
std::shared_ptr<LightActor> LightActor::createLight(const std::string& name, LightActorType type) {
    auto light = std::make_shared<LightActor>(name);
    
    // Configure based on light type
    switch (type) {
        case LightActorType::Directional:
            light->setLightType(LightType::Directional);
            light->setColor(glm::vec3(1.0f, 0.95f, 0.8f)); // Warm sunlight
            light->setIntensity(1.0f);
            light->setDirection(glm::vec3(0.5f, -1.0f, 0.5f)); // 45-degree angle
            break;
            
        case LightActorType::Spot:
            light->setLightType(LightType::Spot);
            light->setColor(glm::vec3(0.9f, 0.9f, 1.0f)); // Cool white
            light->setIntensity(1.2f);
            light->setRange(15.0f);
            light->setConeAngles(15.0f, 30.0f);
            break;
            
        case LightActorType::Area:
            light->setLightType(LightType::Area);
            light->setColor(glm::vec3(0.8f, 0.8f, 0.8f)); // Neutral white
            light->setIntensity(0.8f);
            light->setDimensions(2.0f, 1.0f);
            break;
            
        case LightActorType::Point:
        default:
            light->setLightType(LightType::Point);
            light->setColor(glm::vec3(1.0f, 1.0f, 1.0f)); // White
            light->setIntensity(1.0f);
            light->setRange(10.0f);
            break;
    }
    
    return light;
}

LightActor::LightActor(const std::string& name) : Actor(name) {
    // Add a light component by default
    ensureLightComponent();
}

void LightActor::ensureLightComponent() {
    auto lightComp = getComponent<LightComponent>();
    if (!lightComp) {
        addComponent<LightComponent>();
    }
}

LightComponent* LightActor::getLightComponent() const {
    return getComponent<LightComponent>().get();
}

void LightActor::setLightType(LightType type) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setType(type);
    }
}

LightType LightActor::getLightType() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getType();
    }
    return LightType::Point; // Default
}

void LightActor::setColor(const glm::vec3& color) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setColor(color);
    }
}

const glm::vec3& LightActor::getColor() const {
    static glm::vec3 defaultColor(1.0f, 1.0f, 1.0f);
    if (auto lightComp = getLightComponent()) {
        return lightComp->getColor();
    }
    return defaultColor;
}

void LightActor::setIntensity(float intensity) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setIntensity(intensity);
    }
}

float LightActor::getIntensity() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getIntensity();
    }
    return 1.0f; // Default
}

void LightActor::setLightEnabled(bool enabled) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setEnabled(enabled);
    }
}

bool LightActor::isLightEnabled() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->isEnabled();
    }
    return true; // Default
}

void LightActor::setRange(float range) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setRange(range);
    }
}

float LightActor::getRange() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getRange();
    }
    return 10.0f; // Default
}

void LightActor::setDirection(const glm::vec3& direction) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setDirection(direction);
    }
}

const glm::vec3& LightActor::getDirection() const {
    static glm::vec3 defaultDir(0.0f, -1.0f, 0.0f);
    if (auto lightComp = getLightComponent()) {
        return lightComp->getDirection();
    }
    return defaultDir;
}

void LightActor::setConeAngles(float inner, float outer) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setInnerConeAngle(inner);
        lightComp->setOuterConeAngle(outer);
    }
}

float LightActor::getInnerConeAngle() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getInnerConeAngle();
    }
    return 15.0f; // Default
}

float LightActor::getOuterConeAngle() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getOuterConeAngle();
    }
    return 45.0f; // Default
}

void LightActor::setDimensions(float width, float height) {
    ensureLightComponent();
    if (auto lightComp = getLightComponent()) {
        lightComp->setWidth(width);
        lightComp->setHeight(height);
    }
}

float LightActor::getWidth() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getWidth();
    }
    return 1.0f; // Default
}

float LightActor::getHeight() const {
    if (auto lightComp = getLightComponent()) {
        return lightComp->getHeight();
    }
    return 1.0f; // Default
}

} // namespace ohao 