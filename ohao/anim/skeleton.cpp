#include "anim/skeleton.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ohao::anim {

namespace {

bool inMask(std::span<const uint8_t> mask, std::size_t joint) { return mask.empty() || (joint < mask.size() && mask[joint] != 0); }

// Normalised linear interpolation along the shorter arc; the operations in a fixed order.
glm::quat nlerp(const glm::quat& a, glm::quat b, float w) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = -b;
    const float u = 1.0f - w;
    const glm::quat q(u * a.w + w * b.w, u * a.x + w * b.x, u * a.y + w * b.y, u * a.z + w * b.z);
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 0.0f ? glm::quat(q.w / len, q.x / len, q.y / len, q.z / len) : a;
}

int componentsOf(ChannelPath p) { return p == ChannelPath::Rotation ? 4 : 3; }

} // namespace

int Skeleton::find(std::string_view jointName) const {
    for (std::size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == jointName) return static_cast<int>(i);
    return -1;
}

void restPose(const Skeleton& skeleton, LocalPose& out) {
    const std::size_t n = skeleton.joints.size();
    out.t.resize(n);
    out.r.resize(n);
    out.s.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.t[i] = skeleton.joints[i].translation;
        out.r[i] = skeleton.joints[i].rotation;
        out.s[i] = skeleton.joints[i].scale;
    }
}

Clip bindClip(Clip clip, const Skeleton& skeleton, std::vector<std::string>* dropped) {
    std::vector<Channel> kept;
    kept.reserve(clip.channels.size());
    for (Channel& c : clip.channels) {
        c.joint = skeleton.find(c.node);
        if (c.joint >= 0) {
            kept.push_back(std::move(c));
        } else if (dropped && std::find(dropped->begin(), dropped->end(), c.node) == dropped->end()) {
            dropped->push_back(c.node);
        }
    }
    clip.channels = std::move(kept);
    return clip;
}

float clipTime(float duration, float t, bool loop) {
    if (duration <= 0.0f) return 0.0f;
    if (!loop) return std::clamp(t, 0.0f, duration);
    if (t < 0.0f) t = 0.0f;
    const float wrapped = std::fmod(t, duration);
    return wrapped < duration ? wrapped : 0.0f;
}

void sampleChannel(const Channel& c, float t, float out[4]) {
    const int n = componentsOf(c.path);
    const std::size_t keys = c.times.size();
    const bool cubic = c.interp == Interpolation::CubicSpline;
    const auto value = [&](std::size_t k) {   // the key's value (CUBICSPLINE: the middle of in, value, out)
        return &c.values[(cubic ? 3 * k + 1 : k) * static_cast<std::size_t>(n)];
    };
    const auto copy = [&](const float* v) {
        for (int i = 0; i < n; ++i) out[i] = v[i];
    };
    if (keys == 0) {
        for (int i = 0; i < 4; ++i) out[i] = (n == 4 && i == 3) || (c.path == ChannelPath::Scale) ? 1.0f : 0.0f;
        return;
    }
    if (keys == 1 || t <= c.times.front()) { copy(value(0)); return; }
    if (t >= c.times.back()) { copy(value(keys - 1)); return; }
    // The key at or before t: times[k] <= t < times[k + 1].
    const std::size_t k = static_cast<std::size_t>(std::upper_bound(c.times.begin(), c.times.end(), t) - c.times.begin()) - 1;
    const float t0 = c.times[k], t1 = c.times[k + 1];
    if (c.interp == Interpolation::Step) { copy(value(k)); return; }
    const float span = t1 - t0;
    const float u = span > 0.0f ? (t - t0) / span : 0.0f;
    if (u == 0.0f) { copy(value(k)); return; }   // on a key: its value, to the bit
    if (c.interp == Interpolation::Linear) {
        const float* a = value(k);
        const float* b = value(k + 1);
        if (n == 4) {
            const glm::quat q = nlerp(glm::quat(a[3], a[0], a[1], a[2]), glm::quat(b[3], b[0], b[1], b[2]), u);
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        } else {
            for (int i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * u;
        }
        return;
    }
    // CUBICSPLINE (glTF): p = (2u³ - 3u² + 1)p0 + (u³ - 2u² + u)m0 + (-2u³ + 3u²)p1 + (u³ - u²)m1, with
    // m0 = span × out-tangent of k and m1 = span × in-tangent of k + 1.
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f, h10 = u3 - 2.0f * u2 + u, h01 = -2.0f * u3 + 3.0f * u2, h11 = u3 - u2;
    const float* p0 = &c.values[(3 * k + 1) * n];
    const float* m0 = &c.values[(3 * k + 2) * n];
    const float* m1 = &c.values[(3 * (k + 1) + 0) * n];
    const float* p1 = &c.values[(3 * (k + 1) + 1) * n];
    for (int i = 0; i < n; ++i) out[i] = h00 * p0[i] + h10 * span * m0[i] + h01 * p1[i] + h11 * span * m1[i];
    if (n == 4) {
        const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (len > 0.0f)
            for (int i = 0; i < 4; ++i) out[i] /= len;
    }
}

void sampleClip(const Clip& clip, float t, LocalPose& pose) {
    float v[4];
    for (const Channel& c : clip.channels) {
        if (c.joint < 0 || c.joint >= static_cast<int>(pose.t.size())) continue;
        sampleChannel(c, t, v);
        switch (c.path) {
        case ChannelPath::Translation: pose.t[c.joint] = glm::vec3(v[0], v[1], v[2]); break;
        case ChannelPath::Rotation: pose.r[c.joint] = glm::quat(v[3], v[0], v[1], v[2]); break;
        case ChannelPath::Scale: pose.s[c.joint] = glm::vec3(v[0], v[1], v[2]); break;
        }
    }
}

void blendPose(LocalPose& pose, const LocalPose& other, float w, std::span<const uint8_t> mask) {
    const std::size_t n = std::min(pose.t.size(), other.t.size());
    for (std::size_t j = 0; j < n; ++j) {
        if (!inMask(mask, j)) continue;
        if (w >= 1.0f) {
            pose.t[j] = other.t[j];
            pose.r[j] = other.r[j];
            pose.s[j] = other.s[j];
        } else if (w > 0.0f) {
            pose.t[j] = pose.t[j] + (other.t[j] - pose.t[j]) * w;
            pose.r[j] = nlerp(pose.r[j], other.r[j], w);
            pose.s[j] = pose.s[j] + (other.s[j] - pose.s[j]) * w;
        }
    }
}

void addPose(LocalPose& pose, const LocalPose& layer, const LocalPose& rest, float w, std::span<const uint8_t> mask) {
    const std::size_t n = std::min({pose.t.size(), layer.t.size(), rest.t.size()});
    if (w <= 0.0f) return;
    for (std::size_t j = 0; j < n; ++j) {
        if (!inMask(mask, j)) continue;
        pose.t[j] = pose.t[j] + (layer.t[j] - rest.t[j]) * w;
        const glm::quat delta = glm::inverse(rest.r[j]) * layer.r[j];
        const glm::quat d = w >= 1.0f ? delta : nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, w);
        pose.r[j] = glm::normalize(pose.r[j] * d);
        pose.s[j] = pose.s[j] + (layer.s[j] - rest.s[j]) * w;
    }
}

void modelMatrices(const Skeleton& skeleton, const LocalPose& pose, std::vector<glm::mat4>& model) {
    const std::size_t n = skeleton.joints.size();
    model.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        const Joint& joint = skeleton.joints[j];
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), pose.t[j]) * glm::mat4_cast(pose.r[j]) *
                                glm::scale(glm::mat4(1.0f), pose.s[j]);
        model[j] = (joint.parent >= 0 ? model[static_cast<std::size_t>(joint.parent)] : joint.base) * local;
    }
}

void skinPalette(const Skeleton& skeleton, std::span<const glm::mat4> model, std::span<const int> boneToJoint,
                 std::vector<glm::mat4>& palette) {
    palette.resize(boneToJoint.size());
    for (std::size_t b = 0; b < boneToJoint.size(); ++b) {
        const int j = boneToJoint[b];
        palette[b] = j >= 0 && j < static_cast<int>(model.size())
                         ? model[static_cast<std::size_t>(j)] * skeleton.joints[static_cast<std::size_t>(j)].inverseBind
                         : glm::mat4(1.0f);
    }
}

std::vector<int> boneToJointMap(const Skeleton& skeleton, std::span<const std::string> boneNames) {
    std::vector<int> map(boneNames.size(), -1);
    for (std::size_t b = 0; b < boneNames.size(); ++b) map[b] = skeleton.find(boneNames[b]);
    return map;
}

} // namespace ohao::anim
