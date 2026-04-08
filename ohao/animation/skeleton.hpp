#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace ohao {

struct Joint {
    std::string name;
    int parentIndex{-1};              // -1 for root
    glm::mat4 inverseBindMatrix{1.0f}; // Bind pose inverse
    glm::mat4 localTransform{1.0f};    // Current local transform
};

struct Skeleton {
    std::vector<Joint> joints;
    std::vector<glm::mat4> jointMatrices; // Final matrices for GPU (joint * inverseBindMatrix)

    // Walk hierarchy, compute world transforms, then multiply by inverse bind matrices
    void computeJointMatrices();

    // Find joint index by name, returns -1 if not found
    int findJoint(const std::string& name) const;

    // Reset all joints to bind pose
    void resetToBindPose();
};

} // namespace ohao
