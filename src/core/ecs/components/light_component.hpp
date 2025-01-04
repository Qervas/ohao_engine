#pragma once
#include "ecs/component.hpp"
#include <glm/glm.hpp>

namespace ohao {

class LightComponent : public Component {
public:
    enum class LightType { Directional, Point, Spot };

    void onAttach() override;  // Add these declarations
    void onDetach() override;  // Add these declarations

    void setType(LightType type) { this->type = type; }
    void setColor(const glm::vec3& color) { this->color = color; }
    void setIntensity(float intensity) { this->intensity = intensity; }

    LightType getType() const { return type; }
    const glm::vec3& getColor() const { return color; }
    float getIntensity() const { return intensity; }

private:
    LightType type{LightType::Point};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
};

} // namespace ohao
