#pragma once
#include <glm/glm.hpp>

namespace ohao {

class Ray {
public:
    Ray(const glm::vec3& origin, const glm::vec3& direction)
        : origin(origin)
        , direction(glm::normalize(direction))
    {
    }
    
    // Getters
    const glm::vec3& getOrigin() const { return origin; }
    const glm::vec3& getDirection() const { return direction; }
    
    // Get point at distance along ray
    glm::vec3 getPointAtDistance(float distance) const {
        return origin + direction * distance;
    }
    
private:
    glm::vec3 origin;
    glm::vec3 direction; // Normalized
};

} // namespace ohao 