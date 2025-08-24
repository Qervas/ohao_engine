#include "physics_math.hpp"

namespace ohao {
namespace physics {

// === INERTIA TENSOR CALCULATIONS ===
namespace inertia {

glm::mat3 calculateBoxTensor(float mass, const glm::vec3& dimensions) {
    float x2 = dimensions.x * dimensions.x;
    float y2 = dimensions.y * dimensions.y;
    float z2 = dimensions.z * dimensions.z;
    
    float factor = mass / 12.0f;
    
    return glm::mat3(
        factor * (y2 + z2), 0.0f, 0.0f,
        0.0f, factor * (x2 + z2), 0.0f,
        0.0f, 0.0f, factor * (x2 + y2)
    );
}

glm::mat3 calculateSphereTensor(float mass, float radius) {
    float inertia = 0.4f * mass * radius * radius;
    return glm::mat3(
        inertia, 0.0f, 0.0f,
        0.0f, inertia, 0.0f,
        0.0f, 0.0f, inertia
    );
}

glm::mat3 calculateCylinderTensor(float mass, float radius, float height) {
    float r2 = radius * radius;
    float h2 = height * height;
    
    float ixx = mass * (3.0f * r2 + h2) / 12.0f;
    float iyy = mass * r2 / 2.0f;
    float izz = ixx;
    
    return glm::mat3(
        ixx, 0.0f, 0.0f,
        0.0f, iyy, 0.0f,
        0.0f, 0.0f, izz
    );
}

glm::mat3 calculateCapsuleTensor(float mass, float radius, float height) {
    // Approximate capsule as cylinder + 2 hemispheres
    float cylinderHeight = height;
    float sphereMass = mass * 0.2f; // 20% for spheres
    float cylinderMass = mass * 0.8f; // 80% for cylinder
    
    // Cylinder part
    glm::mat3 cylinderInertia = calculateCylinderTensor(cylinderMass, radius, cylinderHeight);
    
    // Sphere parts (simplified)
    glm::mat3 sphereInertia = calculateSphereTensor(sphereMass, radius);
    
    return cylinderInertia + sphereInertia;
}

glm::mat3 calculateInverse(const glm::mat3& tensor) {
    return glm::inverse(tensor);
}

glm::mat3 transformToWorldSpace(const glm::mat3& localTensor, const glm::quat& rotation) {
    glm::mat3 rotMat = glm::mat3_cast(rotation);
    return rotMat * localTensor * glm::transpose(rotMat);
}

} // namespace inertia
} // namespace physics
} // namespace ohao