#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

// Forward declare ufbx types (avoid including ufbx.h in header)
struct ufbx_scene;

namespace ohao {

struct Joint {
    std::string name;
    int parentIndex{-1};              // -1 for root
    glm::mat4 inverseBindMatrix{1.0f}; // Bind pose inverse
    glm::mat4 localTransform{1.0f};    // Current local transform (animated by clips)
    glm::mat4 preTransform{1.0f};      // Non-joint ancestor chain between parent joint and this joint (static)
};

// Full scene node tree (for correct animation traversal)
struct SkeletonNode {
    std::string name;
    glm::mat4 defaultTransform{1.0f};  // Node's own local transform (from scene file)
    glm::mat4 animatedTransform{1.0f}; // Overwritten by animation channels each frame
    bool hasAnimation{false};           // Set true when animation channel updates this node
    int parentNode{-1};                 // Index in nodeTree, -1 = root
    int boneIndex{-1};                  // Index in joints[], -1 = not a bone
    std::vector<int> children;

    // FBX pre-rotation: when PRESERVE_PIVOTS=false, the bind-pose rotation includes
    // pre-rotation that must be re-applied after animation replaces rotation.
    // bindRotationOffset = inverse(animR_frame0) * fullBindRotation
    // Animated rotation = animR * bindRotationOffset
    glm::quat bindRotationOffset{1.0f, 0.0f, 0.0f, 0.0f};
    bool hasRotationOffset{false};
};

struct Skeleton {
    std::vector<Joint> joints;
    std::vector<glm::mat4> jointMatrices; // Final matrices for GPU (joint * inverseBindMatrix)
    glm::mat4 rootTransform{1.0f}; // Transform of non-joint ancestors (e.g., Armature node)
    glm::mat4 globalInverse{1.0f}; // inverse(sceneRoot.transform)

    // Full node tree for animation traversal (FBX/Assimp path)
    std::vector<SkeletonNode> nodeTree;
    int nodeTreeRoot{-1};
    bool useNodeTree{false}; // true = use tree traversal, false = use joint-only walk

    // Walk hierarchy, compute world transforms, then multiply by inverse bind matrices
    void computeJointMatrices();

    // ufbx-based animation evaluation (handles all FBX rotation orders correctly)
    ufbx_scene* ufbxScene{nullptr};      // Original scene (kept alive for evaluation)
    int ufbxAnimIndex{0};                // Which anim stack to play
    float ufbxAnimDuration{0.0f};
    std::vector<uint32_t> ufbxClusterMap; // jointIndex → ufbx cluster typed_id
    glm::mat4 meshBindPoseInverse{1.0f}; // inverse of mesh geometry-to-world at bind pose

    // Evaluate animation at time t using ufbx (fills jointMatrices directly)
    void evaluateUfbx(float time);

    // Find joint index by name, returns -1 if not found
    int findJoint(const std::string& name) const;

    // Reset all joints to bind pose
    void resetToBindPose();
};

} // namespace ohao
