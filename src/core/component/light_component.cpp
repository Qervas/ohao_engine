#include "light_component.hpp"
#include "../actor/actor.hpp"
#include "../scene/scene.hpp"
#include "../component/transform_component.hpp"

namespace ohao {

LightComponent::LightComponent() : Component() {
    // Default initialization is handled by member initializers
}

void LightComponent::initialize() {
    // Register with scene if we have one
    if (auto* actor = getOwner()) {
        if (auto* scene = actor->getScene()) {
            // Each light gets a unique name based on the component ID and owner name
            std::string lightName = actor->getName() + "_light_" + std::to_string(getID());
            
            // Get the actor's transform position
            glm::vec3 position(0.0f);
            if (auto transform = actor->getTransform()) {
                position = transform->getPosition();
            }
            
            // Create a light struct and register with the scene
            Light light;
            light.color = color;
            light.position = position;
            light.intensity = intensity;
            light.enabled = enabled;
            
            scene->addLight(lightName, light);
        }
    }
}

void LightComponent::update(float deltaTime) {
    // Update light position based on actor's position
    if (auto* actor = getOwner()) {
        if (auto* scene = actor->getScene()) {
            // Each light has a unique name based on the component ID and owner name
            std::string lightName = actor->getName() + "_light_" + std::to_string(getID());
            
            // Get the light from the scene
            if (Light* light = scene->getLight(lightName)) {
                // Get the actor's transform position
                glm::vec3 position(0.0f);
                if (auto transform = actor->getTransform()) {
                    position = transform->getPosition();
                }
                
                // Update light properties based on actor transform
                light->position = position;
                light->color = color;
                light->intensity = intensity;
                light->enabled = enabled;
                
                // Update the light in the scene
                scene->updateLight(lightName, *light);
            }
        }
    }
}

void LightComponent::setColor(const glm::vec3& newColor) {
    color = newColor;
}

void LightComponent::setIntensity(float newIntensity) {
    intensity = newIntensity;
}

void LightComponent::setEnabled(bool isEnabled) {
    enabled = isEnabled;
}

void LightComponent::setRange(float newRange) {
    range = newRange;
}

void LightComponent::setType(LightType newType) {
    type = newType;
}

void LightComponent::setDirection(const glm::vec3& newDirection) {
    direction = glm::normalize(newDirection);
}

void LightComponent::setInnerConeAngle(float angle) {
    innerConeAngle = angle;
}

void LightComponent::setOuterConeAngle(float angle) {
    outerConeAngle = angle;
}

void LightComponent::setWidth(float newWidth) {
    width = newWidth;
}

void LightComponent::setHeight(float newHeight) {
    height = newHeight;
}

nlohmann::json LightComponent::serialize() const {
    nlohmann::json data;
    
    data["color"] = {color.r, color.g, color.b};
    data["intensity"] = intensity;
    data["enabled"] = enabled;
    data["range"] = range;
    data["type"] = static_cast<int>(type);
    data["direction"] = {direction.x, direction.y, direction.z};
    data["innerConeAngle"] = innerConeAngle;
    data["outerConeAngle"] = outerConeAngle;
    data["width"] = width;
    data["height"] = height;
    
    return data;
}

void LightComponent::deserialize(const nlohmann::json& data) {
    if (data.contains("color") && data["color"].is_array() && data["color"].size() == 3) {
        color = {
            data["color"][0].get<float>(),
            data["color"][1].get<float>(),
            data["color"][2].get<float>()
        };
    }
    
    if (data.contains("intensity")) {
        intensity = data["intensity"].get<float>();
    }
    
    if (data.contains("enabled")) {
        enabled = data["enabled"].get<bool>();
    }
    
    if (data.contains("range")) {
        range = data["range"].get<float>();
    }
    
    if (data.contains("type")) {
        type = static_cast<LightType>(data["type"].get<int>());
    }
    
    if (data.contains("direction") && data["direction"].is_array() && data["direction"].size() == 3) {
        direction = glm::normalize(glm::vec3(
            data["direction"][0].get<float>(),
            data["direction"][1].get<float>(),
            data["direction"][2].get<float>()
        ));
    }
    
    if (data.contains("innerConeAngle")) {
        innerConeAngle = data["innerConeAngle"].get<float>();
    }
    
    if (data.contains("outerConeAngle")) {
        outerConeAngle = data["outerConeAngle"].get<float>();
    }
    
    if (data.contains("width")) {
        width = data["width"].get<float>();
    }
    
    if (data.contains("height")) {
        height = data["height"].get<float>();
    }
}

std::shared_ptr<Component> LightComponent::clone() const {
    auto clone = std::make_shared<LightComponent>();
    
    // Copy base component properties
    clone->setEnabled(isEnabled());
    
    // Copy light-specific properties
    clone->color = color;
    clone->intensity = intensity;
    clone->enabled = enabled;
    clone->range = range;
    clone->type = type;
    clone->direction = direction;
    clone->innerConeAngle = innerConeAngle;
    clone->outerConeAngle = outerConeAngle;
    clone->width = width;
    clone->height = height;
    
    return clone;
}

} // namespace ohao 