#include "animation_clip.hpp"
#include "skeleton.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <unordered_map>

namespace ohao {

void AnimationClip::sample(float time, Skeleton& skeleton) const {
    // Wrap time to clip duration
    if (duration > 0.0f) {
        time = std::fmod(time, duration);
        if (time < 0.0f) time += duration;
    }

    // Collect per-target TRS updates
    struct TRS {
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        bool hasT{false}, hasR{false}, hasS{false};
        int jointIdx{-1};
        int nodeIdx{-1};
    };

    std::unordered_map<int, TRS> nodeUpdates;   // keyed by nodeTree index
    std::unordered_map<int, TRS> jointUpdates;   // keyed by joint index (legacy path)
    bool useNodes = skeleton.useNodeTree;

    for (const auto& channel : channels) {
        if (channel.timestamps.empty() || channel.values.empty()) continue;

        int keyA = findKeyframe(channel.timestamps, time);
        int keyB = std::min(keyA + 1, static_cast<int>(channel.timestamps.size()) - 1);

        float t = 0.0f;
        if (keyA != keyB) {
            float tA = channel.timestamps[keyA];
            float tB = channel.timestamps[keyB];
            float dt = tB - tA;
            if (dt > 0.0f) {
                t = (time - tA) / dt;
                t = std::clamp(t, 0.0f, 1.0f);
            }
        }

        // Route to node or joint update map
        TRS* update = nullptr;
        if (useNodes && channel.targetNode >= 0) {
            update = &nodeUpdates[channel.targetNode];
            update->nodeIdx = channel.targetNode;
        } else if (channel.targetJoint >= 0 &&
                   channel.targetJoint < static_cast<int>(skeleton.joints.size())) {
            update = &jointUpdates[channel.targetJoint];
            update->jointIdx = channel.targetJoint;
        }
        if (!update) continue;

        switch (channel.property) {
            case AnimationProperty::TRANSLATION:
                update->translation = interpolateVec3(channel, keyA, keyB, t);
                update->hasT = true;
                break;
            case AnimationProperty::ROTATION:
                update->rotation = interpolateQuat(channel, keyA, keyB, t);
                update->hasR = true;
                break;
            case AnimationProperty::SCALE:
                update->scale = interpolateVec3(channel, keyA, keyB, t);
                update->hasS = true;
                break;
        }
    }

    // Apply node updates (FBX/Assimp path)
    for (auto& [nodeIdx, trs] : nodeUpdates) {
        auto& node = skeleton.nodeTree[nodeIdx];

        // Fill missing channels from the node's default transform
        if (!trs.hasT) trs.translation = glm::vec3(node.defaultTransform[3]);
        if (!trs.hasR) trs.rotation = glm::quat_cast(node.defaultTransform);
        if (!trs.hasS) trs.scale = glm::vec3(
            glm::length(glm::vec3(node.defaultTransform[0])),
            glm::length(glm::vec3(node.defaultTransform[1])),
            glm::length(glm::vec3(node.defaultTransform[2])));

        // Apply bind-pose rotation offset (FBX pre-rotation recovery).
        // Without this, animation replaces the full rotation and loses pre-rotation.
        if (trs.hasR && node.hasRotationOffset) {
            trs.rotation = trs.rotation * node.bindRotationOffset;
        }

        node.animatedTransform = glm::translate(glm::mat4(1.0f), trs.translation) *
                                 glm::mat4_cast(trs.rotation) *
                                 glm::scale(glm::mat4(1.0f), trs.scale);
        node.hasAnimation = true;
    }

    // Apply joint updates (legacy GLTF path)
    for (auto& [jointIdx, trs] : jointUpdates) {
        Joint& joint = skeleton.joints[jointIdx];

        if (!trs.hasT) trs.translation = glm::vec3(joint.localTransform[3]);
        if (!trs.hasR) trs.rotation = glm::quat_cast(joint.localTransform);
        if (!trs.hasS) trs.scale = glm::vec3(
            glm::length(glm::vec3(joint.localTransform[0])),
            glm::length(glm::vec3(joint.localTransform[1])),
            glm::length(glm::vec3(joint.localTransform[2])));

        joint.localTransform = glm::translate(glm::mat4(1.0f), trs.translation) *
                               glm::mat4_cast(trs.rotation) *
                               glm::scale(glm::mat4(1.0f), trs.scale);
    }
}

int AnimationClip::findKeyframe(const std::vector<float>& timestamps, float time) {
    // Binary search for the keyframe just before or at 'time'
    if (timestamps.empty()) return 0;
    if (time <= timestamps.front()) return 0;
    if (time >= timestamps.back()) return static_cast<int>(timestamps.size()) - 1;

    auto it = std::upper_bound(timestamps.begin(), timestamps.end(), time);
    if (it == timestamps.begin()) return 0;
    return static_cast<int>(std::distance(timestamps.begin(), it)) - 1;
}

glm::vec3 AnimationClip::interpolateVec3(const AnimationChannel& channel, int keyA, int keyB, float t) {
    glm::vec3 a(channel.values[keyA].x, channel.values[keyA].y, channel.values[keyA].z);
    glm::vec3 b(channel.values[keyB].x, channel.values[keyB].y, channel.values[keyB].z);

    if (channel.interpolation == InterpolationType::STEP) {
        return a;
    }
    return glm::mix(a, b, t);
}

glm::quat AnimationClip::interpolateQuat(const AnimationChannel& channel, int keyA, int keyB, float t) {
    glm::quat a(channel.values[keyA].w, channel.values[keyA].x,
                channel.values[keyA].y, channel.values[keyA].z);
    glm::quat b(channel.values[keyB].w, channel.values[keyB].x,
                channel.values[keyB].y, channel.values[keyB].z);

    if (channel.interpolation == InterpolationType::STEP) {
        return a;
    }
    return glm::slerp(a, b, t);
}

} // namespace ohao
