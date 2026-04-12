#include "animation_clip.hpp"
#include "skeleton.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>

namespace ohao {

void AnimationClip::sample(float time, Skeleton& skeleton) const {
    // Wrap time to clip duration
    if (duration > 0.0f) {
        time = std::fmod(time, duration);
        if (time < 0.0f) time += duration;
    }

    for (const auto& channel : channels) {
        if (channel.targetJoint < 0 ||
            channel.targetJoint >= static_cast<int>(skeleton.joints.size())) {
            continue;
        }
        if (channel.timestamps.empty() || channel.values.empty()) {
            continue;
        }

        Joint& joint = skeleton.joints[channel.targetJoint];

        int keyA = findKeyframe(channel.timestamps, time);
        int keyB = std::min(keyA + 1, static_cast<int>(channel.timestamps.size()) - 1);

        // Compute interpolation factor
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

        // Decompose current local transform to apply channel updates
        // We build the transform from T, R, S components
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        switch (channel.property) {
            case AnimationProperty::TRANSLATION: {
                translation = interpolateVec3(channel, keyA, keyB, t);
                joint.localTransform = glm::translate(glm::mat4(1.0f), translation) *
                                       glm::mat4_cast(glm::quat_cast(joint.localTransform));
                // Preserve existing scale
                break;
            }
            case AnimationProperty::ROTATION: {
                rotation = interpolateQuat(channel, keyA, keyB, t);
                // Extract existing translation
                glm::vec3 existingTranslation(joint.localTransform[3]);
                joint.localTransform = glm::translate(glm::mat4(1.0f), existingTranslation) *
                                       glm::mat4_cast(rotation);
                break;
            }
            case AnimationProperty::SCALE: {
                scale = interpolateVec3(channel, keyA, keyB, t);
                joint.localTransform = joint.localTransform * glm::scale(glm::mat4(1.0f), scale);
                break;
            }
        }
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
