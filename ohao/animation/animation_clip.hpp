#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace ohao {

struct Skeleton;

enum class AnimationProperty {
    TRANSLATION,
    ROTATION,
    SCALE
};

enum class InterpolationType {
    LINEAR,
    STEP,
    CUBIC_SPLINE
};

struct AnimationChannel {
    int targetJoint{-1};              // Index into Skeleton::joints (legacy/GLTF path)
    int targetNode{-1};               // Index into Skeleton::nodeTree (FBX/Assimp path)
    AnimationProperty property{AnimationProperty::TRANSLATION};
    InterpolationType interpolation{InterpolationType::LINEAR};
    std::vector<float> timestamps;
    std::vector<glm::vec4> values; // vec3 for T/S (w unused), quat for R (xyzw)
};

struct AnimationClip {
    std::string name;
    float duration{0.0f};
    std::vector<AnimationChannel> channels;

    // Sample the animation at the given time and apply to skeleton
    void sample(float time, Skeleton& skeleton) const;

private:
    // Find the two keyframes surrounding the given time
    static int findKeyframe(const std::vector<float>& timestamps, float time);

    // Interpolate between keyframes
    static glm::vec3 interpolateVec3(const AnimationChannel& channel, int keyA, int keyB, float t);
    static glm::quat interpolateQuat(const AnimationChannel& channel, int keyA, int keyB, float t);
};

} // namespace ohao
