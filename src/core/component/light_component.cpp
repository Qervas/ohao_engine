#include "light_component.hpp"
#include "core/actor/actor.hpp"

namespace ohao {

LightComponent::LightComponent() 
    : Component()
    , lightType(LightType::Point)
    , lightColor(1.0f, 1.0f, 1.0f)
    , lightIntensity(1.0f)
    , lightRange(10.0f)
    , innerConeAngle(30.0f)
    , outerConeAngle(45.0f)
    , lightDirection(0.0f, -1.0f, 0.0f)
{
}

void LightComponent::initialize() {
    Component::initialize();
}

void LightComponent::update(float deltaTime) {
    Component::update(deltaTime);
    // Update light parameters if needed
}

void LightComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization when serializer is available
}

void LightComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization when deserializer is available
}

} // namespace ohao