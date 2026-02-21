#include "ragdoll.hpp"
#include <iostream>
#include <glm/gtc/quaternion.hpp>

namespace ohao {

Ragdoll::~Ragdoll() {
    cleanup();
}

bool Ragdoll::buildFromSkeleton(const Skeleton& skeleton, physics::PhysicsWorld* world) {
    if (!world || skeleton.joints.empty()) return false;

    cleanup();
    m_world = world;

    // Create a rigid body for each joint in the skeleton
    m_bones.resize(skeleton.joints.size());

    for (size_t i = 0; i < skeleton.joints.size(); i++) {
        const Joint& joint = skeleton.joints[i];
        RagdollBone& bone = m_bones[i];

        bone.jointIndex = static_cast<int>(i);
        bone.halfExtents = estimateBoneExtents(skeleton, static_cast<int>(i));
        bone.mass = (joint.parentIndex < 0) ? 5.0f : 1.0f; // Root is heavier

        // Create collision shape (capsule along bone direction)
        auto shape = physics::collision::ShapeFactory::createBox(bone.halfExtents);

        // Create rigid body (initially kinematic - not affected by physics)
        // Will be switched to dynamic when ragdoll is activated
        auto rigidBody = std::make_shared<physics::dynamics::RigidBody>(nullptr);
        rigidBody->setType(physics::dynamics::RigidBodyType::KINEMATIC);
        rigidBody->setCollisionShape(shape);
        rigidBody->setMass(bone.mass);

        // Position from skeleton bind pose
        // Use the world-space position from the inverse bind matrix inverse
        glm::mat4 worldTransform = glm::inverse(joint.inverseBindMatrix);
        glm::vec3 pos = glm::vec3(worldTransform[3]);
        rigidBody->setPosition(pos);

        bone.rigidBody = rigidBody;
    }

    // Create constraints between parent-child joints
    for (size_t i = 0; i < skeleton.joints.size(); i++) {
        const Joint& joint = skeleton.joints[i];
        if (joint.parentIndex < 0) continue; // Skip root

        RagdollConstraint constraint;
        constraint.parentBoneIndex = joint.parentIndex;
        constraint.childBoneIndex = static_cast<int>(i);

        // Pivot points at the joint location
        glm::mat4 parentWorld = glm::inverse(skeleton.joints[joint.parentIndex].inverseBindMatrix);
        glm::mat4 childWorld = glm::inverse(joint.inverseBindMatrix);

        glm::vec3 parentPos = glm::vec3(parentWorld[3]);
        glm::vec3 childPos = glm::vec3(childWorld[3]);
        glm::vec3 jointPos = childPos; // Joint is at child's origin

        constraint.pivotInParent = jointPos - parentPos;
        constraint.pivotInChild = glm::vec3(0.0f);
        constraint.coneAngle = 0.5f;
        constraint.twistAngle = 0.3f;

        m_constraints.push_back(constraint);
    }

    std::cout << "Ragdoll: Built from skeleton with " << m_bones.size()
              << " bones and " << m_constraints.size() << " constraints" << std::endl;
    return true;
}

void Ragdoll::activate(const glm::vec3& impulse) {
    if (m_active) return;

    for (auto& bone : m_bones) {
        if (bone.rigidBody) {
            bone.rigidBody->setType(physics::dynamics::RigidBodyType::DYNAMIC);
            bone.rigidBody->setAwake(true);

            // Apply impulse (e.g., from bullet impact)
            if (glm::length(impulse) > 0.0f) {
                bone.rigidBody->applyImpulse(impulse * (1.0f / static_cast<float>(m_bones.size())));
            }
        }
    }

    m_active = true;
    std::cout << "Ragdoll: Activated" << std::endl;
}

void Ragdoll::deactivate() {
    if (!m_active) return;

    for (auto& bone : m_bones) {
        if (bone.rigidBody) {
            bone.rigidBody->setType(physics::dynamics::RigidBodyType::KINEMATIC);
            bone.rigidBody->setLinearVelocity(glm::vec3(0.0f));
            bone.rigidBody->setAngularVelocity(glm::vec3(0.0f));
        }
    }

    m_active = false;
    std::cout << "Ragdoll: Deactivated" << std::endl;
}

void Ragdoll::updateSkeleton(Skeleton& skeleton) const {
    if (!m_active) return;

    for (const auto& bone : m_bones) {
        if (!bone.rigidBody || bone.jointIndex < 0 ||
            bone.jointIndex >= static_cast<int>(skeleton.joints.size())) {
            continue;
        }

        // Get physics transform
        glm::vec3 physPos = bone.rigidBody->getPosition();
        glm::quat physRot = bone.rigidBody->getRotation();

        // Convert to local space relative to parent
        Joint& joint = skeleton.joints[bone.jointIndex];
        if (joint.parentIndex >= 0) {
            // Get parent world transform from its rigid body
            const auto& parentBone = m_bones[joint.parentIndex];
            if (parentBone.rigidBody) {
                glm::vec3 parentPos = parentBone.rigidBody->getPosition();
                glm::quat parentRot = parentBone.rigidBody->getRotation();
                glm::quat parentRotInv = glm::inverse(parentRot);

                // Local transform relative to parent
                glm::vec3 localPos = parentRotInv * (physPos - parentPos);
                glm::quat localRot = parentRotInv * physRot;

                joint.localTransform = glm::translate(glm::mat4(1.0f), localPos) *
                                       glm::mat4_cast(localRot);
            }
        } else {
            // Root joint: use world position directly
            joint.localTransform = glm::translate(glm::mat4(1.0f), physPos) *
                                   glm::mat4_cast(physRot);
        }
    }

    // Recompute joint matrices for GPU skinning
    skeleton.computeJointMatrices();
}

void Ragdoll::cleanup() {
    // Remove rigid bodies from physics world
    if (m_world) {
        for (auto& bone : m_bones) {
            if (bone.rigidBody) {
                m_world->removeRigidBody(bone.rigidBody);
            }
        }
    }

    m_bones.clear();
    m_constraints.clear();
    m_active = false;
    m_world = nullptr;
}

glm::vec3 Ragdoll::estimateBoneExtents(const Skeleton& skeleton, int jointIndex) const {
    const Joint& joint = skeleton.joints[jointIndex];

    // Find child joints to estimate bone length
    float boneLength = 0.1f; // Default minimum
    for (size_t i = 0; i < skeleton.joints.size(); i++) {
        if (skeleton.joints[i].parentIndex == jointIndex) {
            glm::mat4 parentWorld = glm::inverse(joint.inverseBindMatrix);
            glm::mat4 childWorld = glm::inverse(skeleton.joints[i].inverseBindMatrix);
            glm::vec3 diff = glm::vec3(childWorld[3]) - glm::vec3(parentWorld[3]);
            float dist = glm::length(diff);
            if (dist > boneLength) boneLength = dist;
        }
    }

    // Bone shape: elongated box along bone direction
    float thickness = boneLength * 0.2f;
    thickness = glm::max(thickness, 0.02f);
    return glm::vec3(thickness, boneLength * 0.5f, thickness);
}

} // namespace ohao
