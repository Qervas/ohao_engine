#include "skeleton.hpp"

namespace ohao {

void Skeleton::computeJointMatrices() {
    size_t jointCount = joints.size();
    if (jointCount == 0) return;

    jointMatrices.resize(jointCount);

    // Compute world transforms by walking the hierarchy
    std::vector<glm::mat4> worldTransforms(jointCount);

    for (size_t i = 0; i < jointCount; ++i) {
        const Joint& joint = joints[i];
        if (joint.parentIndex < 0) {
            worldTransforms[i] = joint.localTransform;
        } else {
            worldTransforms[i] = worldTransforms[joint.parentIndex] * joint.localTransform;
        }
        // Final matrix = worldTransform * inverseBindMatrix
        jointMatrices[i] = worldTransforms[i] * joint.inverseBindMatrix;
    }
}

int Skeleton::findJoint(const std::string& name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Skeleton::resetToBindPose() {
    for (auto& joint : joints) {
        joint.localTransform = glm::mat4(1.0f);
    }
    computeJointMatrices();
}

} // namespace ohao
