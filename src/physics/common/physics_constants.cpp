#include "physics_constants.hpp"
#include "physics/utils/physics_math.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace inertia {

glm::mat3 calculateBoxTensor(float mass, const glm::vec3& dimensions) {
    // For a box with dimensions (width, height, depth), the diagonal inertia tensor is:
    // Ixx = (1/12) * m * (h² + d²)
    // Iyy = (1/12) * m * (w² + d²)  
    // Izz = (1/12) * m * (w² + h²)
    
    float w2 = dimensions.x * dimensions.x;
    float h2 = dimensions.y * dimensions.y;
    float d2 = dimensions.z * dimensions.z;
    float factor = mass / 12.0f;
    
    return glm::mat3(
        factor * (h2 + d2), 0.0f, 0.0f,
        0.0f, factor * (w2 + d2), 0.0f,
        0.0f, 0.0f, factor * (w2 + h2)
    );
}

glm::mat3 calculateSphereTensor(float mass, float radius) {
    // For a solid sphere: I = (2/5) * m * r²
    float inertia = (2.0f / 5.0f) * mass * radius * radius;
    
    return glm::mat3(
        inertia, 0.0f, 0.0f,
        0.0f, inertia, 0.0f,
        0.0f, 0.0f, inertia
    );
}

glm::mat3 calculateCylinderTensor(float mass, float radius, float height) {
    // For a solid cylinder (Z-axis aligned):
    // Ixx = Iyy = (1/12) * m * (3r² + h²)
    // Izz = (1/2) * m * r²
    
    float r2 = radius * radius;
    float h2 = height * height;
    float ixy = (mass / 12.0f) * (3.0f * r2 + h2);
    float izz = 0.5f * mass * r2;
    
    return glm::mat3(
        ixy, 0.0f, 0.0f,
        0.0f, ixy, 0.0f,
        0.0f, 0.0f, izz
    );
}

glm::mat3 calculateCapsuleTensor(float mass, float radius, float height) {
    // Capsule = cylinder + 2 hemispheres
    // Approximate as a cylinder for simplicity (more accurate calculation would be complex)
    float totalHeight = height + 2.0f * radius;
    return calculateCylinderTensor(mass, radius, totalHeight);
}

glm::mat3 transformToWorldSpace(const glm::mat3& localTensor, const glm::quat& rotation) {
    // Transform inertia tensor: I_world = R * I_local * R^T
    glm::mat3 rotMatrix = glm::mat3_cast(rotation);
    return rotMatrix * localTensor * glm::transpose(rotMatrix);
}

glm::mat3 calculateInverse(const glm::mat3& tensor) {
    // For diagonal tensors (most common case), inverse is straightforward
    glm::mat3 result(0.0f);
    
    // Check if it's a diagonal matrix
    bool isDiagonal = (glm::abs(tensor[0][1]) < constants::EPSILON && 
                      glm::abs(tensor[0][2]) < constants::EPSILON &&
                      glm::abs(tensor[1][0]) < constants::EPSILON &&
                      glm::abs(tensor[1][2]) < constants::EPSILON &&
                      glm::abs(tensor[2][0]) < constants::EPSILON &&
                      glm::abs(tensor[2][1]) < constants::EPSILON);
    
    if (isDiagonal) {
        // Simple diagonal inversion
        result[0][0] = (glm::abs(tensor[0][0]) > constants::EPSILON) ? 1.0f / tensor[0][0] : 0.0f;
        result[1][1] = (glm::abs(tensor[1][1]) > constants::EPSILON) ? 1.0f / tensor[1][1] : 0.0f;
        result[2][2] = (glm::abs(tensor[2][2]) > constants::EPSILON) ? 1.0f / tensor[2][2] : 0.0f;
    } else {
        // General matrix inversion using GLM
        result = glm::inverse(tensor);
    }
    
    return result;
}

glm::mat3 combine(const glm::mat3& tensorA, float massA, const glm::vec3& offsetA,
                 const glm::mat3& tensorB, float massB, const glm::vec3& offsetB) {
    // Parallel axis theorem: I_total = I_A + I_B + parallel_axis_correction
    
    // Calculate center of mass offset
    float totalMass = massA + massB;
    glm::vec3 centerOfMass = (massA * offsetA + massB * offsetB) / totalMass;
    
    // Calculate parallel axis corrections
    glm::vec3 rA = offsetA - centerOfMass;
    glm::vec3 rB = offsetB - centerOfMass;
    
    // Parallel axis theorem contribution: m * (r² * I - r ⊗ r)
    auto parallelAxisCorrection = [](float mass, const glm::vec3& r) -> glm::mat3 {
        float r2 = math::lengthSquared(r);
        glm::mat3 identity(1.0f);
        glm::mat3 outerProduct = glm::outerProduct(r, r);
        return mass * (r2 * identity - outerProduct);
    };
    
    glm::mat3 correctionA = parallelAxisCorrection(massA, rA);
    glm::mat3 correctionB = parallelAxisCorrection(massB, rB);
    
    return tensorA + tensorB + correctionA + correctionB;
}

} // namespace inertia
} // namespace physics
} // namespace ohao