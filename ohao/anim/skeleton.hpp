#pragma once
// Skeletons, clips and local poses (sub-project 10, docs/specs/2026-10-28-animation-design.md,
// "The data model"). No ECS and no Vulkan: ohao_anim depends on core, glm and simdjson only.
//
// A Skeleton is a glTF skin's joints, parents before children, each with its rest TRS (the
// node's) and inverse bind matrix; a root joint carries its non-joint ancestors' transform
// (`base`), so joint matrices are in the glTF scene's space, where the skin's inverse binds are.
// A Clip is a glTF animation bound to one skeleton: per-joint channels, keys as authored.
// Sampling is float, in a fixed order: the same clip, time and pose give the same bits.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::anim {

struct Joint {
    std::string name;
    int parent = -1;   // a joint index, or -1 for a root joint
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::mat4 inverseBind{1.0f};
    glm::mat4 base{1.0f};   // root joints: the rest transform of their non-joint ancestors
};

struct Skeleton {
    std::string name;            // the glTF skin's name ("#<index>" if unnamed)
    std::vector<Joint> joints;   // parents before children
    [[nodiscard]] int find(std::string_view jointName) const;   // -1 if absent
};

enum class ChannelPath : uint8_t { Translation, Rotation, Scale };
enum class Interpolation : uint8_t { Linear, Step, CubicSpline };

struct Channel {
    std::string node;   // the target node's name (bound to a joint by name)
    int joint = -1;     // after binding: the skeleton's joint index
    ChannelPath path = ChannelPath::Rotation;
    Interpolation interp = Interpolation::Linear;
    std::vector<float> times;    // seconds, ascending
    std::vector<float> values;   // 3 (T, S) or 4 (R as x, y, z, w) a key; CUBICSPLINE: in, value, out a key
};

struct ClipEvent {
    float t = 0.0f;   // seconds into the clip
    std::string name;
};

struct Clip {
    std::string name;
    float duration = 0.0f;   // the last key's time over every channel
    std::vector<Channel> channels;
    std::vector<ClipEvent> events;   // sorted by t
};

// Joint-local TRS for every joint of a skeleton.
struct LocalPose {
    std::vector<glm::vec3> t;
    std::vector<glm::quat> r;
    std::vector<glm::vec3> s;
};

void restPose(const Skeleton& skeleton, LocalPose& out);

// A clip's channels bound to a skeleton's joints by name; channels whose node is not a joint are
// dropped, each named once in `dropped`.
[[nodiscard]] Clip bindClip(Clip clip, const Skeleton& skeleton, std::vector<std::string>* dropped = nullptr);

// The time a clip is sampled at: looped (wrapped into [0, duration)) or clamped to [0, duration].
[[nodiscard]] float clipTime(float duration, float t, bool loop);

// Writes the clip's channels at time t (already wrapped or clamped) into `pose`; joints the clip
// has no channel for keep what `pose` holds. Before the first key: the first value; after the
// last: the last.
void sampleClip(const Clip& clip, float t, LocalPose& pose);

// One channel at time t: 3 or 4 floats into `out` (rotations normalised).
void sampleChannel(const Channel& channel, float t, float out[4]);

// pose = pose blended toward `other` by w on the masked joints (all when `mask` is empty):
// lerp for translation and scale, normalised nlerp along the shorter arc for rotation.
void blendPose(LocalPose& pose, const LocalPose& other, float w, std::span<const uint8_t> mask);

// pose += w × (layer − rest) on the masked joints: translation and scale by difference, rotation
// by the layer's rotation relative to the rest's, nlerped from identity by w.
void addPose(LocalPose& pose, const LocalPose& layer, const LocalPose& rest, float w, std::span<const uint8_t> mask);

// Joint matrices in the skeleton's (glTF scene) space: base × parent × T × R × S.
void modelMatrices(const Skeleton& skeleton, const LocalPose& pose, std::vector<glm::mat4>& model);

// The skinning palette in a mesh's bone order: palette[b] = model[joint] × inverseBind[joint],
// joint = boneToJoint[b] (identity for a bone that names no joint, -1).
void skinPalette(const Skeleton& skeleton, std::span<const glm::mat4> model, std::span<const int> boneToJoint,
                 std::vector<glm::mat4>& palette);

// A mesh's bone order (ModelLoader's bone names) to the skeleton's joints, by name; -1 for a bone
// that names no joint.
[[nodiscard]] std::vector<int> boneToJointMap(const Skeleton& skeleton, std::span<const std::string> boneNames);

} // namespace ohao::anim
