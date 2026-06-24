#include "light_component.hpp"
#include "scene/actor/actor.hpp"
// Registration moved to central file

namespace ohao {

LightComponent::LightComponent() 
    : Component()
    , lightType(LightType::Sphere)
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

} // namespace ohao