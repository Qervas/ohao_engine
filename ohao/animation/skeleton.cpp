#include "skeleton.hpp"
#include "ufbx.h"

namespace ohao {

void Skeleton::computeJointMatrices() {
    size_t jointCount = joints.size();
    if (jointCount == 0) return;

    jointMatrices.resize(jointCount);

    if (useNodeTree && !nodeTree.empty()) {
        // Full node tree traversal (correct for FBX/Assimp with intermediate nodes).
        // Walk the tree top-down, accumulating global transforms.
        // For bone nodes, compute: globalInverse * globalTransform * offsetMatrix
        std::vector<glm::mat4> nodeGlobals(nodeTree.size(), glm::mat4(1.0f));

        for (size_t i = 0; i < nodeTree.size(); i++) {
            const auto& node = nodeTree[i];
            glm::mat4 local = node.hasAnimation ? node.animatedTransform : node.defaultTransform;

            if (node.parentNode < 0) {
                nodeGlobals[i] = local;
            } else {
                nodeGlobals[i] = nodeGlobals[node.parentNode] * local;
            }

            if (node.boneIndex >= 0 && node.boneIndex < static_cast<int>(jointCount)) {
                jointMatrices[node.boneIndex] = nodeGlobals[i]
                                              * joints[node.boneIndex].inverseBindMatrix;
            }
        }

        // Reset animation flags for next frame
        for (auto& node : nodeTree) {
            node.hasAnimation = false;
        }
    } else {
        // Joint-only walk (legacy path for GLTF or simple skeletons)
        std::vector<glm::mat4> worldTransforms(jointCount);

        for (size_t i = 0; i < jointCount; ++i) {
            const Joint& joint = joints[i];
            glm::mat4 local = joint.preTransform * joint.localTransform;
            if (joint.parentIndex < 0) {
                worldTransforms[i] = rootTransform * local;
            } else {
                worldTransforms[i] = worldTransforms[joint.parentIndex] * local;
            }
            jointMatrices[i] = worldTransforms[i] * joint.inverseBindMatrix;
        }
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
    if (useNodeTree) {
        for (auto& node : nodeTree) {
            node.hasAnimation = false;
        }
    }
    computeJointMatrices();
}

static glm::mat4 ufbxToGlm(const ufbx_matrix& m) {
    return glm::mat4(
        static_cast<float>(m.cols[0].x), static_cast<float>(m.cols[0].y), static_cast<float>(m.cols[0].z), 0.0f,
        static_cast<float>(m.cols[1].x), static_cast<float>(m.cols[1].y), static_cast<float>(m.cols[1].z), 0.0f,
        static_cast<float>(m.cols[2].x), static_cast<float>(m.cols[2].y), static_cast<float>(m.cols[2].z), 0.0f,
        static_cast<float>(m.cols[3].x), static_cast<float>(m.cols[3].y), static_cast<float>(m.cols[3].z), 1.0f
    );
}

void Skeleton::evaluateUfbx(float time) {
    if (!ufbxScene || joints.empty()) return;

    // Wrap time to animation duration
    if (ufbxAnimDuration > 0.0f) {
        time = std::fmod(time, ufbxAnimDuration);
        if (time < 0.0f) time += ufbxAnimDuration;
    }

    // Get the animation to evaluate
    const ufbx_anim* anim = ufbxScene->anim;
    if (ufbxAnimIndex < static_cast<int>(ufbxScene->anim_stacks.count)) {
        anim = ufbxScene->anim_stacks.data[ufbxAnimIndex]->anim;
    }

    // Evaluate entire scene at time t — ufbx handles all FBX complexity
    ufbx_error error;
    ufbx_scene* evaluated = ufbx_evaluate_scene(ufbxScene, anim, static_cast<double>(time), nullptr, &error);
    if (!evaluated) return;

    jointMatrices.resize(joints.size());

    // For each joint, compute: bone_node_to_world * geometry_to_bone
    for (size_t i = 0; i < joints.size(); i++) {
        if (i >= ufbxClusterMap.size()) {
            jointMatrices[i] = glm::mat4(1.0f);
            continue;
        }

        uint32_t clusterId = ufbxClusterMap[i];
        if (clusterId >= evaluated->skin_clusters.count) {
            jointMatrices[i] = glm::mat4(1.0f);
            continue;
        }

        const ufbx_skin_cluster* cluster = evaluated->skin_clusters.data[clusterId];
        // meshBindPoseInverse * geometry_to_world = mesh-local deformation
        // (shader applies pc.model for world transform on top)
        jointMatrices[i] = meshBindPoseInverse * ufbxToGlm(cluster->geometry_to_world);
    }

    ufbx_free_scene(evaluated);
}

} // namespace ohao
