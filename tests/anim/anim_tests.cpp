// anim_tests (sub-project 10, docs/specs/2026-10-28-animation-design.md, "Test plan"): test 22
// (assets and sampling) and test 23's step (the graph's state machine, events, restores), on the
// generated rig (tools/anim/make_test_rig.py, tests/anim/fixtures/). CPU only.
#include "anim/anim_cache.hpp"
#include "anim/anim_graph.hpp"
#include "anim/gltf_anim.hpp"
#include "anim/skeleton.hpp"
#include "common/harness.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "reflect/registry.hpp"
#include "scene/asset/model_loader.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace ohao;
namespace fs = std::filesystem;

namespace {

const std::string kRig = "tests/anim/fixtures/rig.glb";
const std::string kGraph = "tests/anim/fixtures/rig.animgraph.json";

std::optional<std::vector<uint8_t>> readFile(const std::string& path) {
    std::ifstream f(fs::path(OHAO_SOURCE_DIR) / path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

anim::AnimCache& cache() {
    static anim::AnimCache c(readFile);
    return c;
}

std::shared_ptr<const anim::BoundGraph> rigGraph() {
    auto g = cache().graph(kGraph);
    if (!g) {
        std::printf("\n      graph error: %s %s", g.error().path.c_str(), g.error().message.c_str());
        return nullptr;
    }
    return g.value();
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

bool near(float a, float b, float eps = 1e-6f) { return std::abs(a - b) <= eps; }
bool nearQ(const glm::quat& a, const glm::quat& b, float eps = 1e-6f) {
    return (near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps) && near(a.w, b.w, eps)) ||
           (near(a.x, -b.x, eps) && near(a.y, -b.y, eps) && near(a.z, -b.z, eps) && near(a.w, -b.w, eps));
}
glm::quat qz(float s, float c) { return glm::quat(c, 0.0f, 0.0f, s); }   // the generator's table values
bool samePose(const anim::LocalPose& a, const anim::LocalPose& b, std::size_t joint) {
    return std::memcmp(&a.t[joint], &b.t[joint], sizeof(glm::vec3)) == 0 &&
           std::memcmp(&a.r[joint], &b.r[joint], sizeof(glm::quat)) == 0 &&
           std::memcmp(&a.s[joint], &b.s[joint], sizeof(glm::vec3)) == 0;
}

const anim::GltfAnimData* rigData() {
    static std::shared_ptr<const anim::GltfAnimData> d;
    if (!d) {
        auto r = cache().gltf(kRig);
        if (r) d = r.value();
        else std::printf("\n      rig: %s", r.error().message.c_str());
    }
    return d.get();
}

const anim::Clip* rigClip(const std::string& name) {
    for (const auto& c : rigData()->clips)
        if (c.name == name) return &c;
    return nullptr;
}

std::string base64(const std::vector<uint8_t>& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0u) |
                           (i + 2 < in.size() ? uint32_t(in[i + 2]) : 0u);
        out += k[(n >> 18) & 63];
        out += k[(n >> 12) & 63];
        out += i + 1 < in.size() ? k[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? k[n & 63] : '=';
    }
    return out;
}

// ---- test 22: assets and sampling ---------------------------------------------------------------

void t22_gltf_skin_read() {
    const anim::GltfAnimData* d = rigData();
    CHECK(d != nullptr, "the rig reads");
    if (!d) return;
    CHECK(d->skins.size() == 1 && d->skins[0].name == "RigSkin", "one skin, RigSkin");
    if (d->skins.empty()) return;
    const anim::Skeleton& s = d->skins[0];
    const char* names[4] = {"root", "lower", "middle", "upper"};
    CHECK(s.joints.size() == 4, "four joints");
    for (int k = 0; k < 4 && k < static_cast<int>(s.joints.size()); ++k) {
        const anim::Joint& j = s.joints[k];
        CHECK(j.name == names[k] && j.parent == k - 1, (std::string("joint ") + names[k] + ": name and parent").c_str());
        CHECK(j.translation == glm::vec3(0.0f, k == 0 ? 0.0f : 0.5f, 0.0f), "rest translation");
        CHECK(j.inverseBind == glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f * k, 0.0f)), "inverse bind");
        CHECK(j.base == glm::mat4(1.0f), "no ancestors above the root joint");
    }
    CHECK(s.find("middle") == 2 && s.find("nope") == -1, "find by name");
}

void t22_gltf_clip_read() {
    const anim::GltfAnimData* d = rigData();
    if (!d) { CHECK(false, "the rig reads"); return; }
    CHECK(d->clips.size() == 4, "four clips");
    const anim::Clip* wave = rigClip("wave");
    const anim::Clip* step = rigClip("step");
    const anim::Clip* spline = rigClip("spline");
    const anim::Clip* raise = rigClip("raise");
    CHECK(wave && step && spline && raise, "wave, raise, step, spline");
    if (!wave || !step || !spline || !raise) return;
    CHECK(wave->duration == 2.0f && raise->duration == 1.0f && step->duration == 1.0f && spline->duration == 1.0f,
          "durations: the last key");
    CHECK(wave->channels.size() == 3 && wave->channels[0].node == "lower" &&
              wave->channels[0].path == anim::ChannelPath::Rotation &&
              wave->channels[0].interp == anim::Interpolation::Linear,
          "wave: three LINEAR rotation channels, lower first");
    if (wave->channels.size() == 3) {
        const auto& c = wave->channels[0];
        CHECK(c.times == (std::vector<float>{0.0f, 0.5f, 1.0f, 1.5f, 2.0f}), "wave's key times");
        CHECK(c.values.size() == 20 && c.values[6] == 0.19509032f && c.values[7] == 0.98078528f &&
                  c.values[14] == -0.19509032f,
              "wave's key values as stored (float32 of the generator's table)");
    }
    CHECK(step->channels.size() == 2 && step->channels[0].interp == anim::Interpolation::Step &&
              step->channels[1].path == anim::ChannelPath::Scale,
          "step: STEP rotation and scale");
    CHECK(spline->channels.size() == 1 && spline->channels[0].interp == anim::Interpolation::CubicSpline &&
              spline->channels[0].path == anim::ChannelPath::Translation && spline->channels[0].values.size() == 18,
          "spline: CUBICSPLINE translation, in/value/out per key");
}

void t22_sample_linear_step_spline() {
    const anim::Skeleton& s = rigData()->skins[0];
    float out[4];
    // LINEAR rotation halfway between identity and 22.5 degrees: normalised nlerp.
    const anim::Clip wave = anim::bindClip(*rigClip("wave"), s);
    anim::sampleChannel(wave.channels[0], 0.25f, out);
    const glm::quat q0(1, 0, 0, 0), q1 = qz(0.19509032f, 0.98078528f);
    const glm::quat mid = glm::normalize(glm::quat(0.5f * q0.w + 0.5f * q1.w, 0, 0, 0.5f * q0.z + 0.5f * q1.z));
    CHECK(nearQ(glm::quat(out[3], out[0], out[1], out[2]), mid), "LINEAR: normalised nlerp at 0.25 s");
    anim::sampleChannel(wave.channels[0], 0.5f, out);
    CHECK(out[2] == 0.19509032f && out[3] == 0.98078528f, "LINEAR: a key's value exactly at its time");
    // STEP: the earlier key's value until the next key's time.
    const anim::Clip step = anim::bindClip(*rigClip("step"), s);
    anim::sampleChannel(step.channels[0], 0.4999f, out);
    CHECK(out[2] == 0.0f && out[3] == 1.0f, "STEP: before 0.5 s, the first key");
    anim::sampleChannel(step.channels[0], 0.5f, out);
    CHECK(out[2] == 0.38268343f && out[3] == 0.92387953f, "STEP: at 0.5 s, the second key");
    anim::sampleChannel(step.channels[1], 0.75f, out);
    CHECK(out[0] == 1.5f && out[1] == 1.5f && out[2] == 1.5f, "STEP scale");
    // CUBICSPLINE: the glTF Hermite form, p(0.5) = 0.5·0.25 + (0.125 - 0.25)·0.5 = 0.0625.
    const anim::Clip spline = anim::bindClip(*rigClip("spline"), s);
    anim::sampleChannel(spline.channels[0], 0.5f, out);
    CHECK(near(out[1], 0.0625f) && out[0] == 0.0f && out[2] == 0.0f, "CUBICSPLINE at 0.5 s");
    anim::sampleChannel(spline.channels[0], 1.0f, out);
    CHECK(out[1] == 0.25f, "CUBICSPLINE at the last key: its value");
    anim::sampleChannel(spline.channels[0], 2.0f, out);
    CHECK(out[1] == 0.25f, "after the last key: the last value");
    // A whole clip into a pose: only its joints change.
    anim::LocalPose pose;
    anim::restPose(s, pose);
    anim::sampleClip(anim::bindClip(*rigClip("raise"), s), 1.0f, pose);
    CHECK(nearQ(pose.r[1], qz(0.70710678f, 0.70710678f)) && pose.r[0] == glm::quat(1, 0, 0, 0) &&
              pose.r[2] == glm::quat(1, 0, 0, 0),
          "raise: lower at 90 degrees, the others at rest");
}

void t22_loop_and_clamp() {
    CHECK(anim::clipTime(2.0f, 2.5f, true) == 0.5f, "a looping clip wraps");
    CHECK(anim::clipTime(2.0f, 4.0f, true) == 0.0f, "two loops: back at 0");
    CHECK(anim::clipTime(2.0f, 2.5f, false) == 2.0f, "a one-shot clamps at its end");
    CHECK(anim::clipTime(2.0f, 1.25f, false) == 1.25f, "inside: unchanged");
    CHECK(anim::clipTime(0.0f, 1.0f, true) == 0.0f, "an empty clip: 0");
}

void t22_palette_identity_at_bind() {
    const anim::Skeleton& s = rigData()->skins[0];
    anim::LocalPose pose;
    anim::restPose(s, pose);
    std::vector<glm::mat4> model, palette;
    anim::modelMatrices(s, pose, model);
    const std::vector<int> identityMap{0, 1, 2, 3};
    anim::skinPalette(s, model, identityMap, palette);
    bool identity = palette.size() == 4;
    for (const glm::mat4& m : palette)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) identity = identity && near(m[c][r], c == r ? 1.0f : 0.0f);
    CHECK(identity, "the rest pose's palette is identity to 1e-6");
    CHECK(model.size() == 4 && near(model[3][3][1], 1.5f), "upper's joint at y 1.5");
}

void t22_assimp_joint_remap() {
    auto m = ModelLoader::loadMesh((fs::path(OHAO_SOURCE_DIR) / kRig).string(), "Column");
    CHECK(m.has_value() && m.value(), "loadMesh(rig, Column)");
    if (!m || !m.value()) return;
    const Model& model = *m.value();
    CHECK(model.vertices.size() == 96 && model.indices.size() == 144, "96 vertices, 48 triangles");
    CHECK(model.boneNames.size() == 4, "four bone names");
    const anim::Skeleton& s = rigData()->skins[0];
    const std::vector<int> map = anim::boneToJointMap(s, model.boneNames);
    bool named = map.size() == model.boneNames.size();
    for (std::size_t b = 0; b < map.size(); ++b) named = named && map[b] >= 0 && s.joints[map[b]].name == model.boneNames[b];
    CHECK(named, "every bone maps to the joint of its name");
    // Mesh-local (not node-baked) positions; each vertex's heaviest bone is its box's joint.
    float yMin = 1e9f, yMax = -1e9f;
    bool weights = true;
    for (const Vertex& v : model.vertices) {
        yMin = std::min(yMin, v.position.y);
        yMax = std::max(yMax, v.position.y);
        const int box = std::min(3, static_cast<int>(std::floor(v.position.y * 2.0f - 1e-4f)));
        const int heavy = v.boneWeights.x >= v.boneWeights.y ? v.boneIndices.x : v.boneIndices.y;
        const int joint = map[static_cast<std::size_t>(heavy)];
        weights = weights && (joint == std::max(0, box) || joint == std::max(0, box) + 1) &&
                  near(v.boneWeights.x + v.boneWeights.y + v.boneWeights.z + v.boneWeights.w, 1.0f, 1e-5f);
    }
    CHECK(yMin == 0.0f && yMax == 2.0f, "the column spans y 0..2 in its own space");
    CHECK(weights, "weights sum to 1 and name the box's joints");
}

void t22_morph_refused() {
    // A .gltf with a data URI: two times and two morph weights.
    std::vector<uint8_t> bin(16);
    const float data[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    std::memcpy(bin.data(), data, 16);
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Face"}],)"
        R"("buffers":[{"byteLength":16,"uri":"data:application/octet-stream;base64,)" + base64(bin) + R"("}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":8}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
        R"({"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR"}],)"
        R"("animations":[{"name":"smile","samplers":[{"input":0,"output":1}],)"
        R"("channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}]})";
    const auto r = anim::readGltfAnimation(std::span(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
    CHECK(r.has_value(), "the file reads");
    if (!r) return;
    CHECK(r.value().clips.size() == 1 && r.value().clips[0].channels.empty(), "the weights channel is dropped");
    bool warned = false;
    for (const auto& w : r.value().warnings) warned = warned || w.find("morph") != std::string::npos;
    CHECK(warned, "with a warning naming morph targets");
}

void t22_blend1d_by_param() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    const int moves = g->state(0, "moves");
    CHECK(moves >= 0, "state 'moves'");
    std::vector<anim::LayerState> layers = anim::initialLayers(*g);
    layers[0].state = moves;
    layers[0].steps = 30;   // 30 steps at alpha 1
    anim::Params p;
    p.values.assign(g->paramNames.size(), 0.0f);
    anim::PoseScratch scratch;
    anim::LocalPose atZero, atOne, atHalf, wave, step;
    p.values[g->param("speed")] = 0.0f;
    anim::evaluatePose(*g, layers, p, 1.0f, atZero, scratch);
    p.values[g->param("speed")] = 1.0f;
    anim::evaluatePose(*g, layers, p, 1.0f, atOne, scratch);
    p.values[g->param("speed")] = 0.5f;
    anim::evaluatePose(*g, layers, p, 1.0f, atHalf, scratch);
    anim::restPose(g->skeleton, wave);
    anim::restPose(g->skeleton, step);
    const float t = 30.0f * anim::kStepSeconds;
    for (const auto& c : g->clips) {
        if (c.name == "wave") anim::sampleClip(c.clip, t, wave);
        if (c.name == "step") anim::sampleClip(c.clip, t, step);
    }
    bool zeroIsWave = true, oneIsStep = true;
    for (std::size_t j = 0; j < 4; ++j) {
        zeroIsWave = zeroIsWave && nearQ(atZero.r[j], wave.r[j]);
        oneIsStep = oneIsStep && nearQ(atOne.r[j], step.r[j]);
    }
    CHECK(zeroIsWave, "speed 0: the first point's clip");
    CHECK(oneIsStep, "speed 1: the second point's clip");
    glm::quat expect = glm::normalize(glm::quat(0.5f * wave.r[3].w + 0.5f * step.r[3].w, 0, 0, 0.5f * wave.r[3].z + 0.5f * step.r[3].z));
    CHECK(nearQ(atHalf.r[3], expect, 1e-5f), "speed 0.5: halfway");
}

void t22_layers_mask_additive() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    std::vector<anim::LayerState> layers = anim::initialLayers(*g);
    layers[0].steps = 20;
    anim::Params p;
    p.values.assign(g->paramNames.size(), 0.0f);
    anim::PoseScratch scratch;
    anim::LocalPose still, nod;
    anim::evaluatePose(*g, layers, p, 1.0f, still, scratch);
    layers[1].state = g->state(1, "nod");
    layers[1].steps = 31;   // the step clip's 45-degree key
    anim::evaluatePose(*g, layers, p, 1.0f, nod, scratch);
    CHECK(samePose(still, nod, 0) && samePose(still, nod, 1) && samePose(still, nod, 2),
          "the additive layer leaves joints outside its mask exact");
    CHECK(!samePose(still, nod, 3), "and turns the masked joint");
    // blendPose with a mask: the unmasked joints unchanged, bit for bit.
    anim::LocalPose a = still, b = nod;
    const std::vector<uint8_t> mask{0, 0, 0, 1};
    anim::blendPose(a, b, 0.5f, mask);
    CHECK(samePose(a, still, 0) && samePose(a, still, 2), "blendPose: unmasked joints untouched");
}

void t22_graph_errors() {
    const auto skeleton = rigData()->skins[0];
    const auto bindJson = [&](const std::string& json) {
        auto parsed = anim::parseAnimGraph(json);
        if (!parsed) return parsed.error();
        auto bound = anim::bindGraph(std::move(parsed.value()), skeleton, rigData()->clips);
        return bound ? std::vector<anim::GraphError>{} : bound.error();
    };
    const std::string head = R"({"skeleton":{"file":"tests/anim/fixtures/rig.glb","skin":"RigSkin"},)"
                             R"("clips":{"wave":{"file":"tests/anim/fixtures/rig.glb","animation":"wave","loop":true},)"
                             R"("raise":{"file":"tests/anim/fixtures/rig.glb","animation":"raise","loop":false,)"
                             R"("events":[{"t":EVT,"name":"x"}]}},"params":{"speed":"float"},"layers":[{"name":"body",)";
    const auto graph = [&](const std::string& evt, const std::string& rest) {
        std::string h = head;
        h.replace(h.find("EVT"), 3, evt);
        return h + rest;
    };
    const std::string ok = R"("initial":"wave","states":{"wave":{"clip":"wave"},"up":{"clip":"raise"}},)"
                           R"("transitions":[{"from":"wave","to":"up","when":{"param":"speed","op":">","value":1}},)"
                           R"({"from":"up","to":"wave","when":{"clipEnd":true}}]}]})";
    CHECK(bindJson(graph("0.5", ok)).empty(), "a sound graph binds");
    const auto has = [](const std::vector<anim::GraphError>& e, const std::string& path, const std::string& what) {
        for (const auto& x : e)
            if (x.path == path && x.message.find(what) != std::string::npos) return true;
        for (const auto& x : e) std::printf("\n      %s: %s", x.path.c_str(), x.message.c_str());
        return false;
    };
    {
        std::string bad = ok;
        bad.replace(bad.find("\"param\":\"speed\""), 15, "\"param\":\"sped\"");
        CHECK(has(bindJson(graph("0.5", bad)), "/layers/0/transitions/0/when/param", "unknown parameter"),
              "an unknown parameter, with its path");
    }
    {
        std::string bad = ok;
        const std::string exit = R"({"from":"up","to":"wave","when":{"clipEnd":true}})";
        bad.replace(bad.find(exit), exit.size(), R"({"from":"wave","to":"wave","when":{"clipEnd":true}})");
        CHECK(has(bindJson(graph("0.5", bad)), "/layers/0/states/up", "no way out"), "a one-shot state no transition leaves");
    }
    {
        std::string bad = ok;
        bad.insert(bad.find("\"initial\""), R"("mask":["elbow"],)");
        CHECK(has(bindJson(graph("0.5", bad)), "/layers/0/mask/0", "not a joint"), "a mask joint not in the skeleton");
    }
    CHECK(has(bindJson(graph("1.5", ok)), "/clips/raise/events/0/t", "outside"), "an event outside its clip");
}

// ---- test 23: the step ----------------------------------------------------------------------------

anim::Params params(const anim::BoundGraph& g, float speed = 0.0f, float raised = 0.0f) {
    anim::Params p;
    p.values.assign(g.paramNames.size(), 0.0f);
    p.values[g.param("speed")] = speed;
    p.values[g.param("raised")] = raised;
    return p;
}

void t23_transition_on_param() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    auto layers = anim::initialLayers(*g);
    std::vector<anim::FiredEvent> fired;
    anim::stepLayer(*g, 0, layers[0], params(*g), fired);
    CHECK(layers[0].state == g->state(0, "wave") && layers[0].steps == 1, "no condition holds: wave, one step");
    anim::stepLayer(*g, 0, layers[0], params(*g, 0.0f, 1.0f), fired);
    CHECK(layers[0].state == g->state(0, "raise") && layers[0].steps == 1 && layers[0].from == g->state(0, "wave") &&
              layers[0].fromSteps == 2 && layers[0].fade == 6 && layers[0].faded == 1,
          "raised = 1: raise, faded from wave over 6 steps");
}

void t23_transition_on_event() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    auto layers = anim::initialLayers(*g);
    std::vector<anim::FiredEvent> fired;
    anim::Params p = params(*g);
    anim::stepLayer(*g, 1, layers[1], p, fired);
    CHECK(layers[1].state == g->state(1, "still"), "no event: still");
    p.events = {"nod"};
    anim::stepLayer(*g, 1, layers[1], p, fired);
    CHECK(layers[1].state == g->state(1, "nod") && layers[1].steps == 1, "the event 'nod': nod");
}

void t23_transition_on_clip_end() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    anim::LayerState s;
    s.state = g->state(0, "raise");
    std::vector<anim::FiredEvent> fired;
    const anim::Params p = params(*g, 0.0f, 1.0f);
    int steps = 0;
    while (s.state == g->state(0, "raise") && steps < 200) {
        anim::stepLayer(*g, 0, s, p, fired);
        ++steps;
    }
    std::printf("\n      left raise after %d steps", steps);
    CHECK(s.state == g->state(0, "held") && steps == 61, "the 1 s clip ends at step 60; the next step leaves it");
}

void t23_fade_steps() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    auto layers = anim::initialLayers(*g);
    std::vector<anim::FiredEvent> fired;
    const anim::Params up = params(*g, 0.0f, 1.0f);
    anim::stepLayer(*g, 0, layers[0], up, fired);   // into raise, faded 1
    for (int i = 2; i <= 6; ++i) {
        anim::stepLayer(*g, 0, layers[0], up, fired);
        CHECK(layers[0].faded == i && layers[0].from >= 0, "the fade advances a step at a time");
    }
    anim::stepLayer(*g, 0, layers[0], up, fired);
    CHECK(layers[0].from == -1 && layers[0].fade == 0, "the step after the fade completes, the partner is gone");
    // The pose at faded 3 of 6, alpha 1: halfway between the two states' poses... weighted 3/6.
    auto mid = anim::initialLayers(*g);
    mid[0] = {g->state(0, "raise"), 3, g->state(0, "wave"), 40, 6, 3};
    anim::PoseScratch scratch;
    anim::LocalPose blended, a, b;
    anim::evaluatePose(*g, mid, up, 1.0f, blended, scratch);
    anim::restPose(g->skeleton, a);
    anim::restPose(g->skeleton, b);
    for (const auto& c : g->clips) {
        if (c.name == "raise") anim::sampleClip(c.clip, 3.0f * anim::kStepSeconds, a);
        if (c.name == "wave") anim::sampleClip(c.clip, 40.0f * anim::kStepSeconds, b);
    }
    anim::blendPose(b, a, 0.5f, {});
    CHECK(nearQ(blended.r[1], b.r[1], 1e-5f), "faded 3 of 6 at alpha 1: weight 0.5");
}

void t23_events_once_per_crossing() {
    auto g = rigGraph();
    if (!g) { CHECK(false, "the rig graph binds"); return; }
    auto layers = anim::initialLayers(*g);
    std::vector<int> at;
    for (int step = 1; step <= 360; ++step) {
        std::vector<anim::FiredEvent> fired;
        anim::stepLayer(*g, 0, layers[0], params(*g), fired);
        for (const auto& e : fired)
            if (e.name == "tick") at.push_back(step);
    }
    std::printf("\n      tick at steps:");
    for (int s : at) std::printf(" %d", s);
    CHECK(at.size() == 3, "a 2 s loop over 6 s: three ticks");
    CHECK(at.size() == 3 && at[1] - at[0] == 120 && at[2] - at[1] == 120, "one loop (120 steps) apart, none twice");
}

// Step 3: a clip's rate from a parameter (`rateFrom`): its anchor (an event, else its end) lands
// `param` seconds into the state, so the arms' mag_in lands on the weapon's reload time.
void t23_rate_from_param() {
    const auto skeleton = rigData()->skins[0];
    const auto bind = [&](const std::string& rateFrom) -> std::shared_ptr<anim::BoundGraph> {
        const std::string json =
            R"({"skeleton":{"file":"tests/anim/fixtures/rig.glb","skin":"RigSkin"},)"
            R"("clips":{"wave":{"file":"tests/anim/fixtures/rig.glb","animation":"wave","loop":true},)"
            R"("raise":{"file":"tests/anim/fixtures/rig.glb","animation":"raise","loop":false,"rateFrom":)" +
            rateFrom + R"(,"events":[{"t":0.25,"name":"early"},{"t":0.5,"name":"x"}]}},)"
            R"("params":{"len":"float"},"layers":[{"name":"body","initial":"up",)"
            R"("states":{"up":{"clip":"raise"},"wave":{"clip":"wave"}},)"
            R"("transitions":[{"from":"up","to":"wave","when":{"clipEnd":true}}]}]})";
        auto parsed = anim::parseAnimGraph(json);
        if (!parsed) {
            for (const auto& e : parsed.error()) std::printf("\n      %s: %s", e.path.c_str(), e.message.c_str());
            return nullptr;
        }
        auto bound = anim::bindGraph(std::move(parsed.value()), skeleton, rigData()->clips);
        if (!bound) {
            for (const auto& e : bound.error()) std::printf("\n      %s: %s", e.path.c_str(), e.message.c_str());
            return nullptr;
        }
        return std::make_shared<anim::BoundGraph>(std::move(bound.value()));
    };
    // The steps until `name` fires (or the state is left, for "" ), with len = `len` seconds.
    const auto stepsTo = [](const anim::BoundGraph& g, float len, const std::string& name) {
        anim::Params p;
        p.values.assign(g.paramNames.size(), 0.0f);
        p.values[static_cast<std::size_t>(g.param("len"))] = len;
        auto layers = anim::initialLayers(g);
        for (int step = 1; step <= 600; ++step) {
            std::vector<anim::FiredEvent> fired;
            anim::stepLayer(g, 0, layers[0], p, fired);
            if (name.empty() && layers[0].state != g.state(0, "up")) return step;
            for (const auto& e : fired)
                if (e.name == name) return step;
        }
        return -1;
    };
    const auto atX = bind(R"({"param":"len","at":"x"})");
    const auto atEnd = bind(R"({"param":"len"})");
    CHECK(atX && atEnd, "rateFrom {param, at} and {param} bind");
    if (!atX || !atEnd) return;
    const int x15 = stepsTo(*atX, 1.5f, "x"), x20 = stepsTo(*atX, 2.0f, "x"), early = stepsTo(*atX, 2.0f, "early");
    std::printf("\n      x (0.5 s) at step %d with len 1.5, %d with len 2.0; early (0.25 s) at %d", x15, x20, early);
    CHECK(x15 == 90 && x20 == 120, "the anchor event lands len seconds in: 90 steps for 1.5 s, 120 for 2.0 s");
    CHECK(early == 60, "the other events scale with it: 0.25 of 0.5 lands at half of 2.0 s");
    const int end = stepsTo(*atEnd, 2.0f, "");
    CHECK(end == 121, "no anchor: the clip's end lands len seconds in (left on the next step)");
    CHECK(stepsTo(*atX, 0.0f, "x") == 30, "len 0 (no driver wrote it): the clip's own rate");
    // Errors with their paths: an unknown parameter, an event the clip does not have.
    auto noParam = anim::parseAnimGraph(std::string(R"({"skeleton":{"file":"tests/anim/fixtures/rig.glb"},)") +
                                        R"("clips":{"raise":{"file":"tests/anim/fixtures/rig.glb","animation":"raise",)"
                                        R"("rateFrom":{"param":"nope","at":"zz"}}},"layers":[{"initial":"up",)"
                                        R"("states":{"up":{"clip":"raise"}},"transitions":[{"from":"up","to":"up",)"
                                        R"("when":{"clipEnd":true}}]}]})");
    bool param = false, event = false;
    if (noParam) {
        auto bound = anim::bindGraph(std::move(noParam.value()), skeleton, rigData()->clips);
        if (!bound)
            for (const auto& e : bound.error()) {
                param |= e.path == "/clips/raise/rateFrom/param" && e.message.find("unknown parameter") != std::string::npos;
                event |= e.path == "/clips/raise/rateFrom/at" && e.message.find("no event") != std::string::npos;
            }
    }
    CHECK(param && event, "rateFrom's unknown parameter and missing event, each with its path");
}

void t23_events_in_step_order() {
    // Through the world: two animated entities, their events in id order within a step.
    ecs::World w;
    ecs::FileAssetResolver files(OHAO_SOURCE_DIR);
    ecs::installAnimation(w, files);
    const entt::entity a = w.createWithId(0xa1, "A");
    const entt::entity b = w.createWithId(0xb2, "B");
    for (entt::entity e : {b, a}) w.registry().emplace<ecs::Animator>(e, ecs::Animator{{kGraph, {}}, {}});
    std::vector<ecs::AnimEvent> all;
    for (int i = 0; i < 20; ++i) {
        w.simulate(anim::kStepSeconds);
        for (const auto& e : ecs::animationSystem(w)->stepEvents()) all.push_back(e);
    }
    CHECK(all.size() == 2 && all[0].entity == 0xa1 && all[1].entity == 0xb2 && all[0].step == 15 && all[0].seq + 1 == all[1].seq,
          "both ticks at step 15, A before B, consecutive in the log");
    uint64_t dropped = 0;
    const auto since = ecs::animationSystem(w)->eventsSince(0, 10, dropped);
    CHECK(since.size() == 2 && dropped == 0 && since[1].name == "tick", "the log by cursor");
}

std::vector<ecs::AnimLayerState> animatorOf(ecs::World& w, entt::entity e) { return w.registry().get<ecs::Animator>(e).layers; }
bool sameLayers(const std::vector<ecs::AnimLayerState>& a, const std::vector<ecs::AnimLayerState>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].state != b[i].state || a[i].steps != b[i].steps || a[i].from != b[i].from || a[i].fromSteps != b[i].fromSteps ||
            a[i].fade != b[i].fade || a[i].faded != b[i].faded)
            return false;
    return true;
}

void t23_restore_mid_clip() {
    ecs::World w;
    ecs::FileAssetResolver files(OHAO_SOURCE_DIR);
    ecs::installAnimation(w, files);
    const entt::entity e = w.createWithId(0xe1, "Rig");
    w.registry().emplace<ecs::Animator>(e, ecs::Animator{{kGraph, {}}, {}});
    auto& params = w.registry().emplace<ecs::AnimParams>(e);
    params.set("raised", 1.0f);   // no driver: the params hold
    for (int i = 0; i < 3; ++i) w.simulate(anim::kStepSeconds);
    CHECK(animatorOf(w, e).size() == 2 && animatorOf(w, e)[0].state == "raise" && animatorOf(w, e)[0].from == "wave",
          "three steps in: raise, fading from wave");
    const ecs::Snapshot mid = ecs::takeSnapshot(w, engineTypes());
    const auto run = [&]() {
        std::vector<std::string> names;
        for (int i = 0; i < 90; ++i) {
            w.simulate(anim::kStepSeconds);
            for (const auto& ev : ecs::animationSystem(w)->stepEvents()) names.push_back(ev.name + "@" + std::to_string(ev.step));
        }
        ecs::Snapshot table;
        return std::make_tuple(animatorOf(w, e), names, ecs::hashWorld(w, engineTypes(), table));
    };
    const auto [layers1, events1, hash1] = run();
    ecs::restoreSnapshot(w, mid);
    CHECK(animatorOf(w, e).size() == 2 && animatorOf(w, e)[0].state == "raise" && animatorOf(w, e)[0].steps == 3,
          "restored to the snapshot's layers");
    const auto [layers2, events2, hash2] = run();
    CHECK(sameLayers(layers1, layers2) && hash1 == hash2, "the same Animator and world.hash after a restore mid-fade");
    CHECK(layers1.size() == 2 && layers1[0].state == "held", "and the run reached held");
}

void t23_pose_never_in_world() {
    ecs::World w;
    ecs::FileAssetResolver files(OHAO_SOURCE_DIR);
    ecs::installAnimation(w, files);
    const entt::entity e = w.createWithId(0xe1, "Rig");
    w.registry().emplace<ecs::Animator>(e, ecs::Animator{{kGraph, {}}, {}});
    for (int i = 0; i < 10; ++i) w.simulate(anim::kStepSeconds);
    ecs::Snapshot t0, t1, t2;
    w.setRenderAlpha(1.0f);
    w.sync();
    const uint64_t h1 = ecs::hashWorld(w, engineTypes(), t0);
    w.setRenderAlpha(0.3f);
    w.sync();
    const uint64_t h2 = ecs::hashWorld(w, engineTypes(), t1);
    CHECK(h1 == h2, "world.hash is the same at any alpha");
    const auto* pose = w.registry().try_get<ecs::AnimPose>(e);
    CHECK(pose == nullptr || pose->palette.empty(), "no SkinnedMesh: no palette (nothing to skin)");
    (void)t2;
}

} // namespace

int main() {
    std::printf("anim_tests\n");
    RUN(t22_gltf_skin_read);
    RUN(t22_gltf_clip_read);
    RUN(t22_sample_linear_step_spline);
    RUN(t22_loop_and_clamp);
    RUN(t22_palette_identity_at_bind);
    RUN(t22_assimp_joint_remap);
    RUN(t22_morph_refused);
    RUN(t22_blend1d_by_param);
    RUN(t22_layers_mask_additive);
    RUN(t22_graph_errors);
    RUN(t23_transition_on_param);
    RUN(t23_transition_on_event);
    RUN(t23_transition_on_clip_end);
    RUN(t23_fade_steps);
    RUN(t23_events_once_per_crossing);
    RUN(t23_rate_from_param);
    RUN(t23_events_in_step_order);
    RUN(t23_restore_mid_clip);
    RUN(t23_pose_never_in_world);
    return ohao::test::finish("anim_tests");
}
