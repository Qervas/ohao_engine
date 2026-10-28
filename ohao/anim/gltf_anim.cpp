#include "anim/gltf_anim.hpp"

#include <simdjson.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace ohao::anim {

namespace {

using simdjson::dom::array;
using simdjson::dom::element;
using simdjson::dom::object;
using R = Result<GltfAnimData, GltfAnimError>;

GltfAnimError invalid(std::string m) { return GltfAnimError{"E_ASSET_INVALID", std::move(m)}; }

struct Chunks {
    std::string_view json;
    std::span<const uint8_t> bin;
};

bool chunks(std::span<const uint8_t> bytes, Chunks& out, std::string& why) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        if (bytes.size() < 20) { why = "a truncated glTF binary header"; return false; }
        uint32_t version, length, jsonLength, jsonType;
        std::memcpy(&version, bytes.data() + 4, 4);
        std::memcpy(&length, bytes.data() + 8, 4);
        std::memcpy(&jsonLength, bytes.data() + 12, 4);
        std::memcpy(&jsonType, bytes.data() + 16, 4);
        if (version != 2) { why = "glTF binary version " + std::to_string(version) + ", not 2"; return false; }
        if (length > bytes.size() || 20ull + jsonLength > bytes.size() || jsonType != 0x4E4F534Au) {
            why = "a malformed glTF binary";
            return false;
        }
        out.json = std::string_view(reinterpret_cast<const char*>(bytes.data()) + 20, jsonLength);
        const std::size_t binAt = 20ull + jsonLength;
        if (binAt + 8 <= length) {
            uint32_t binLength, binType;
            std::memcpy(&binLength, bytes.data() + binAt, 4);
            std::memcpy(&binType, bytes.data() + binAt + 4, 4);
            if (binType == 0x004E4942u && binAt + 8 + binLength <= bytes.size())
                out.bin = bytes.subspan(binAt + 8, binLength);
        }
        return true;
    }
    out.json = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool base64Decode(std::string_view in, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

bool getIndex(element e, int& out) {
    int64_t v;
    if (e.get_int64().get(v) != simdjson::SUCCESS || v < 0 || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

// obj[key] as an index; false when absent or not a non-negative integer.
bool keyIndex(element obj, std::string_view key, int& out) {
    element e;
    return obj[key].get(e) == simdjson::SUCCESS && getIndex(e, out);
}

bool getFloats(element e, float* out, std::size_t n) {
    array a;
    if (e.get_array().get(a) != simdjson::SUCCESS || a.size() != n) return false;
    std::size_t i = 0;
    for (element x : a) {
        double d;
        if (x.get_double().get(d) != simdjson::SUCCESS) return false;
        out[i++] = static_cast<float>(d);
    }
    return true;
}

struct View { int buffer = 0; std::size_t offset = 0, length = 0, stride = 0; };
struct Accessor { int view = -1; std::size_t offset = 0, count = 0; int componentType = 0, components = 0; bool normalized = false; };

int componentsOf(std::string_view type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

int componentSize(int t) {
    switch (t) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: return 0;
    }
}

float componentValue(const uint8_t* p, int type, bool normalized) {
    switch (type) {
    case 5126: { float f; std::memcpy(&f, p, 4); return f; }
    case 5120: { int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max(v / 127.0f, -1.0f) : float(v); }
    case 5121: return normalized ? *p / 255.0f : float(*p);
    case 5122: { int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max(v / 32767.0f, -1.0f) : float(v); }
    case 5123: { uint16_t v; std::memcpy(&v, p, 2); return normalized ? v / 65535.0f : float(v); }
    case 5125: { uint32_t v; std::memcpy(&v, p, 4); return float(v); }
    default: return 0.0f;
    }
}

struct NodeInfo {
    std::string name;
    int parent = -1;
    glm::vec3 t{0.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 s{1.0f};
    glm::mat4 matrix{1.0f};   // the rest local transform
};

} // namespace

Result<GltfAnimData, GltfAnimError> readGltfAnimation(std::span<const uint8_t> fileBytes) {
    Chunks c;
    std::string why;
    if (!chunks(fileBytes, c, why)) return R::err(invalid(why));
    simdjson::dom::parser parser;
    const simdjson::padded_string padded(c.json);
    element root;
    if (const auto err = parser.parse(padded).get(root); err != simdjson::SUCCESS)
        return R::err(invalid(std::string("not glTF JSON: ") + simdjson::error_message(err)));
    object doc;
    if (root.get_object().get(doc) != simdjson::SUCCESS) return R::err(invalid("not glTF JSON"));

    GltfAnimData data;

    // Buffers: the binary chunk, or a base64 data URI.
    std::vector<std::vector<uint8_t>> owned;
    std::vector<std::span<const uint8_t>> buffers;
    {
        array a;
        if (doc["buffers"].get_array().get(a) == simdjson::SUCCESS)
            for (element b : a) {
                std::string_view uri;
                if (b["uri"].get_string().get(uri) != simdjson::SUCCESS) {
                    buffers.push_back(c.bin);
                    continue;
                }
                if (uri.substr(0, 5) != "data:")
                    return R::err(GltfAnimError{"E_ASSET_UNSUPPORTED", "an external buffer ('" + std::string(uri) +
                                                                           "'): export a .glb, or embed the buffer"});
                const std::size_t comma = uri.find(',');
                if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
                    return R::err(invalid("a data URI that is not base64"));
                owned.emplace_back();
                if (!base64Decode(uri.substr(comma + 1), owned.back())) return R::err(invalid("bad base64 in a data URI"));
                buffers.emplace_back(owned.back());
            }
    }
    // owned's vectors may have moved while it grew: rebuild the spans over them.
    {
        std::size_t k = 0;
        array a;
        if (doc["buffers"].get_array().get(a) == simdjson::SUCCESS) {
            std::size_t i = 0;
            for (element b : a) {
                std::string_view uri;
                if (b["uri"].get_string().get(uri) == simdjson::SUCCESS) buffers[i] = owned[k++];
                ++i;
            }
        }
    }

    std::vector<View> views;
    {
        array a;
        if (doc["bufferViews"].get_array().get(a) == simdjson::SUCCESS)
            for (element v : a) {
                View view;
                int64_t x = 0;
                if (!keyIndex(v, "buffer", view.buffer) || view.buffer >= static_cast<int>(buffers.size()))
                    return R::err(invalid("bufferView " + std::to_string(views.size()) + ": bad buffer"));
                if (v["byteOffset"].get_int64().get(x) == simdjson::SUCCESS) view.offset = static_cast<std::size_t>(x);
                if (v["byteLength"].get_int64().get(x) != simdjson::SUCCESS) return R::err(invalid("bufferView without byteLength"));
                view.length = static_cast<std::size_t>(x);
                if (v["byteStride"].get_int64().get(x) == simdjson::SUCCESS) view.stride = static_cast<std::size_t>(x);
                if (view.offset + view.length > buffers[view.buffer].size())
                    return R::err(invalid("bufferView " + std::to_string(views.size()) + " runs past its buffer"));
                views.push_back(view);
            }
    }

    std::vector<Accessor> accessors;
    {
        array a;
        if (doc["accessors"].get_array().get(a) == simdjson::SUCCESS)
            for (element e : a) {
                Accessor acc;
                int64_t x = 0;
                std::string_view type;
                element sparse;
                if (e["sparse"].get(sparse) == simdjson::SUCCESS)
                    return R::err(GltfAnimError{"E_ASSET_UNSUPPORTED", "sparse accessors are not supported"});
                if (e["bufferView"].get_int64().get(x) == simdjson::SUCCESS) acc.view = static_cast<int>(x);
                if (e["byteOffset"].get_int64().get(x) == simdjson::SUCCESS) acc.offset = static_cast<std::size_t>(x);
                if (e["count"].get_int64().get(x) != simdjson::SUCCESS) return R::err(invalid("accessor without count"));
                acc.count = static_cast<std::size_t>(x);
                if (e["componentType"].get_int64().get(x) != simdjson::SUCCESS) return R::err(invalid("accessor without componentType"));
                acc.componentType = static_cast<int>(x);
                if (e["type"].get_string().get(type) != simdjson::SUCCESS) return R::err(invalid("accessor without type"));
                acc.components = componentsOf(type);
                bool normalized = false;
                if (e["normalized"].get_bool().get(normalized) == simdjson::SUCCESS) acc.normalized = normalized;
                accessors.push_back(acc);
            }
    }
    // An accessor's values as floats (count × components), or an error message.
    const auto floats = [&](int index, int wantComponents, std::vector<float>& out, std::string& err) {
        if (index < 0 || index >= static_cast<int>(accessors.size())) { err = "accessor index out of range"; return false; }
        const Accessor& a = accessors[index];
        if (a.components != wantComponents) {
            err = "accessor " + std::to_string(index) + " has " + std::to_string(a.components) + " components, not " +
                  std::to_string(wantComponents);
            return false;
        }
        const int size = componentSize(a.componentType);
        if (size == 0) { err = "accessor " + std::to_string(index) + ": unknown component type"; return false; }
        out.assign(a.count * a.components, 0.0f);
        if (a.view < 0) return true;   // all zeros
        const View& v = views[a.view];
        const std::size_t elem = static_cast<std::size_t>(size) * a.components;
        const std::size_t stride = v.stride ? v.stride : elem;
        if (a.count > 0 && a.offset + stride * (a.count - 1) + elem > v.length) {
            err = "accessor " + std::to_string(index) + " runs past its bufferView";
            return false;
        }
        const uint8_t* base = buffers[v.buffer].data() + v.offset + a.offset;
        for (std::size_t i = 0; i < a.count; ++i)
            for (int k = 0; k < a.components; ++k)
                out[i * a.components + k] = componentValue(base + i * stride + static_cast<std::size_t>(k) * size, a.componentType,
                                                           a.normalized);
        return true;
    };

    // Nodes: names, rest transforms and parents (glTF indices).
    std::vector<NodeInfo> nodes;
    {
        array a;
        if (doc["nodes"].get_array().get(a) == simdjson::SUCCESS) {
            for (element n : a) {
                NodeInfo info;
                std::string_view name;
                info.name = n["name"].get_string().get(name) == simdjson::SUCCESS ? std::string(name)
                                                                                    : "#" + std::to_string(nodes.size());
                element v;
                if (n["matrix"].get(v) == simdjson::SUCCESS) {
                    float m[16];
                    if (!getFloats(v, m, 16)) return R::err(invalid("node '" + info.name + "': bad matrix"));
                    std::memcpy(&info.matrix[0][0], m, sizeof m);
                    info.t = glm::vec3(info.matrix[3]);
                    glm::vec3 cols[3] = {glm::vec3(info.matrix[0]), glm::vec3(info.matrix[1]), glm::vec3(info.matrix[2])};
                    info.s = glm::vec3(glm::length(cols[0]), glm::length(cols[1]), glm::length(cols[2]));
                    for (int k = 0; k < 3; ++k) cols[k] /= info.s[k];
                    info.r = glm::normalize(glm::quat_cast(glm::mat3(cols[0], cols[1], cols[2])));
                } else {
                    float q[4] = {0, 0, 0, 1};
                    if (n["translation"].get(v) == simdjson::SUCCESS && !getFloats(v, &info.t.x, 3))
                        return R::err(invalid("node '" + info.name + "': bad translation"));
                    if (n["rotation"].get(v) == simdjson::SUCCESS && !getFloats(v, q, 4))
                        return R::err(invalid("node '" + info.name + "': bad rotation"));
                    if (n["scale"].get(v) == simdjson::SUCCESS && !getFloats(v, &info.s.x, 3))
                        return R::err(invalid("node '" + info.name + "': bad scale"));
                    info.r = glm::quat(q[3], q[0], q[1], q[2]);
                    info.matrix = glm::translate(glm::mat4(1.0f), info.t) * glm::mat4_cast(info.r) *
                                  glm::scale(glm::mat4(1.0f), info.s);
                }
                nodes.push_back(std::move(info));
            }
            std::size_t i = 0;
            for (element n : a) {
                array children;
                if (n["children"].get_array().get(children) == simdjson::SUCCESS)
                    for (element ch : children) {
                        int k;
                        if (!getIndex(ch, k) || k >= static_cast<int>(nodes.size()))
                            return R::err(invalid("node '" + nodes[i].name + "': child index out of range"));
                        if (nodes[k].parent >= 0) return R::err(invalid("node '" + nodes[k].name + "' has two parents"));
                        nodes[k].parent = static_cast<int>(i);
                    }
                ++i;
            }
        }
    }
    const auto depthOf = [&](int n) {
        int d = 0;
        for (int p = nodes[n].parent; p >= 0 && d <= static_cast<int>(nodes.size()); p = nodes[p].parent) ++d;
        return d;
    };

    // Skins.
    {
        array a;
        if (doc["skins"].get_array().get(a) == simdjson::SUCCESS)
            for (element s : a) {
                Skeleton sk;
                std::string_view name;
                sk.name = s["name"].get_string().get(name) == simdjson::SUCCESS ? std::string(name)
                                                                                 : "#" + std::to_string(data.skins.size());
                array joints;
                if (s["joints"].get_array().get(joints) != simdjson::SUCCESS || joints.size() == 0)
                    return R::err(invalid("skin '" + sk.name + "' has no joints"));
                std::vector<int> nodeOfJoint;
                for (element j : joints) {
                    int n;
                    if (!getIndex(j, n) || n >= static_cast<int>(nodes.size()))
                        return R::err(invalid("skin '" + sk.name + "': joint index out of range"));
                    nodeOfJoint.push_back(n);
                }
                std::vector<float> ibm(nodeOfJoint.size() * 16, 0.0f);
                for (std::size_t k = 0; k < nodeOfJoint.size(); ++k)
                    for (int d = 0; d < 4; ++d) ibm[k * 16 + d * 5] = 1.0f;
                element ibmIndex;
                if (s["inverseBindMatrices"].get(ibmIndex) == simdjson::SUCCESS) {
                    int idx;
                    std::string err;
                    if (!getIndex(ibmIndex, idx) || !floats(idx, 16, ibm, err) || ibm.size() != nodeOfJoint.size() * 16)
                        return R::err(invalid("skin '" + sk.name + "': bad inverseBindMatrices" + (err.empty() ? "" : ": " + err)));
                }
                // Parents before children: by depth, then skin order.
                std::vector<std::size_t> order(nodeOfJoint.size());
                for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
                std::stable_sort(order.begin(), order.end(),
                                 [&](std::size_t x, std::size_t y) { return depthOf(nodeOfJoint[x]) < depthOf(nodeOfJoint[y]); });
                std::vector<int> jointOfNode(nodes.size(), -1);
                for (std::size_t k = 0; k < order.size(); ++k) jointOfNode[nodeOfJoint[order[k]]] = static_cast<int>(k);
                for (std::size_t k = 0; k < order.size(); ++k) {
                    const int n = nodeOfJoint[order[k]];
                    Joint j;
                    j.name = nodes[n].name;
                    j.translation = nodes[n].t;
                    j.rotation = nodes[n].r;
                    j.scale = nodes[n].s;
                    std::memcpy(&j.inverseBind[0][0], &ibm[order[k] * 16], 16 * sizeof(float));
                    // The nearest ancestor that is a joint; a root joint's non-joint ancestors are its base.
                    int p = nodes[n].parent;
                    while (p >= 0 && jointOfNode[p] < 0) p = nodes[p].parent;
                    if (p >= 0) {
                        j.parent = jointOfNode[p];
                        if (nodes[n].parent != p)
                            data.warnings.push_back("skin '" + sk.name + "': joint '" + j.name +
                                                    "' has a non-joint node between it and its parent joint; that node's transform is ignored");
                    } else {
                        glm::mat4 base(1.0f);
                        for (int q = nodes[n].parent; q >= 0; q = nodes[q].parent) base = nodes[q].matrix * base;
                        j.base = base;
                    }
                    sk.joints.push_back(std::move(j));
                }
                data.skins.push_back(std::move(sk));
            }
    }

    // Animations.
    {
        array a;
        if (doc["animations"].get_array().get(a) == simdjson::SUCCESS)
            for (element an : a) {
                Clip clip;
                std::string_view name;
                clip.name = an["name"].get_string().get(name) == simdjson::SUCCESS ? std::string(name)
                                                                                    : "#" + std::to_string(data.clips.size());
                array samplers, channels;
                if (an["samplers"].get_array().get(samplers) != simdjson::SUCCESS ||
                    an["channels"].get_array().get(channels) != simdjson::SUCCESS)
                    return R::err(invalid("animation '" + clip.name + "' has no samplers or channels"));
                std::vector<element> samplerList;
                for (element s : samplers) samplerList.push_back(s);
                bool morphWarned = false;
                for (element ch : channels) {
                    int si, node;
                    std::string_view path;
                    if (!keyIndex(ch, "sampler", si) || si >= static_cast<int>(samplerList.size()))
                        return R::err(invalid("animation '" + clip.name + "': channel sampler out of range"));
                    element target, nodeElement;
                    if (ch["target"].get(target) != simdjson::SUCCESS)
                        return R::err(invalid("animation '" + clip.name + "': a channel without a target"));
                    if (target["node"].get(nodeElement) != simdjson::SUCCESS) continue;   // no target node: ignored (glTF allows it)
                    if (!getIndex(nodeElement, node) || node >= static_cast<int>(nodes.size()))
                        return R::err(invalid("animation '" + clip.name + "': channel node out of range"));
                    if (target["path"].get_string().get(path) != simdjson::SUCCESS)
                        return R::err(invalid("animation '" + clip.name + "': a channel without a path"));
                    if (path == "weights") {
                        if (!morphWarned)
                            data.warnings.push_back("animation '" + clip.name + "': morph-target weights channels dropped (no morph targets in sub-project 10)");
                        morphWarned = true;
                        continue;
                    }
                    Channel channel;
                    channel.node = nodes[node].name;
                    if (path == "translation") channel.path = ChannelPath::Translation;
                    else if (path == "rotation") channel.path = ChannelPath::Rotation;
                    else if (path == "scale") channel.path = ChannelPath::Scale;
                    else return R::err(invalid("animation '" + clip.name + "': unknown channel path '" + std::string(path) + "'"));
                    element s = samplerList[si];
                    std::string_view interp = "LINEAR";
                    (void)s["interpolation"].get_string().get(interp);
                    if (interp == "LINEAR") channel.interp = Interpolation::Linear;
                    else if (interp == "STEP") channel.interp = Interpolation::Step;
                    else if (interp == "CUBICSPLINE") channel.interp = Interpolation::CubicSpline;
                    else return R::err(invalid("animation '" + clip.name + "': unknown interpolation '" + std::string(interp) + "'"));
                    int in, out;
                    std::string err;
                    if (!keyIndex(s, "input", in) || !floats(in, 1, channel.times, err))
                        return R::err(invalid("animation '" + clip.name + "': bad sampler input" + (err.empty() ? "" : ": " + err)));
                    const int comps = channel.path == ChannelPath::Rotation ? 4 : 3;
                    if (!keyIndex(s, "output", out) || !floats(out, comps, channel.values, err))
                        return R::err(invalid("animation '" + clip.name + "': bad sampler output" + (err.empty() ? "" : ": " + err)));
                    const std::size_t perKey = channel.interp == Interpolation::CubicSpline ? 3u : 1u;
                    if (channel.values.size() != channel.times.size() * perKey * static_cast<std::size_t>(comps))
                        return R::err(invalid("animation '" + clip.name + "': output count does not match input"));
                    for (std::size_t k = 1; k < channel.times.size(); ++k)
                        if (channel.times[k] < channel.times[k - 1])
                            return R::err(invalid("animation '" + clip.name + "': key times not ascending"));
                    if (!channel.times.empty()) clip.duration = std::max(clip.duration, channel.times.back());
                    clip.channels.push_back(std::move(channel));
                }
                data.clips.push_back(std::move(clip));
            }
    }
    return R::ok(std::move(data));
}

} // namespace ohao::anim
