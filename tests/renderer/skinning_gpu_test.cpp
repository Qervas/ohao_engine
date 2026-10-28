// skinning_gpu_test (sub-project 10, docs/specs/2026-10-28-animation-design.md, test 24): GPU
// skinning on the generated rig (tools/anim/make_test_rig.py), through the headless host as
// `ohao run` renders it.
//
//   t24_skin_matches_cpu             the deformed range read back equals a CPU skin of the same
//                                    palette (positions and normals, 1e-5 relative)
//   t24_two_instances_two_poses      two instances of one mesh in different poses: each range is
//                                    its own palette's skin, and they differ
//   t24_motion_vectors_of_a_limb     after a step, the previous positions the motion vectors
//                                    read (the copy's boneWeights.xyz) are last frame's skin: the
//                                    swinging limb moved, the foot (joint 0 only) did not
//   t24_rt_sees_the_pose             the raised arm against the bind pose changes the deferred
//                                    frame (G-buffer, RT shadow) and the path tracer's, in the
//                                    same place
//   t24_refit_and_rebuild            a pose changing every frame refits its BLAS, and rebuilds it
//                                    after kBlasRefitsPerRebuild refits
//   t24_unchanged_pose_skips         a still pose runs no skin, bounds or refit work
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "common/harness.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/render_extract.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "reflect/registry.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <process.h>   // _getpid

using namespace ohao;
namespace fs = std::filesystem;

namespace {

int g_passed = 0, g_failed = 0;
std::vector<std::string> g_failures;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        g_failures.push_back(what);
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

constexpr uint64_t kRigId = 0xa17a000000000004ull;
const char* kRigScene = "assets/scenes/anim_rig.scene.json";

// The rig scene with another rig beside the first (x + 1, raised), written per process.
std::string twoRigScene() {
    std::ifstream in(fs::path(OHAO_SOURCE_DIR) / kRigScene);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string second = R"(,
    {
      "id": "a17a000000000005",
      "Name": { "value": "Rig2" },
      "Transform": { "position": [1, 0, 0] },
      "MeshSource": { "source": { "ModelAsset": { "asset": { "path": "tests/anim/fixtures/rig.glb" }, "mesh": "Column" } } },
      "SkinnedMesh": { "file": { "path": "tests/anim/fixtures/rig.glb" }, "skin": "RigSkin" },
      "Animator": { "graph": { "path": "tests/anim/fixtures/rig.animgraph.json" },
        "layers": [ { "state": "held", "steps": 5 }, { "state": "still", "steps": 0 } ] }
    }
  ]
})";
    const auto end = text.rfind("\n  ]");
    text = text.substr(0, end) + second;
    const fs::path out = fs::path(OHAO_BINARY_DIR) / ("skinning_two_rigs_" + std::to_string(_getpid()) + ".scene.json");
    std::ofstream(out, std::ios::binary) << text;
    return out.generic_string();
}

// A headless session on a scene, as `ohao run` renders it (the host before the world).
struct Rig {
    ecs::FileAssetResolver assets{OHAO_SOURCE_DIR};
    host::RenderHost host{assets, api::HostKind::Headless};
    ecs::World world;
    api::Session session{world, engineTypes()};
    bool loaded{false};

    explicit Rig(const std::string& scene) {
        session.setAssetResolver(&assets);
        ecs::installAnimation(world, assets);
        host.bind(world);
        session.setHost(&host);
        const std::string reply = session.call("scene.load", "{\"replace\":true,\"path\":\"" + scene + "\"}");
        loaded = reply.rfind("{\"result\"", 0) == 0;
        if (!loaded) std::printf("  scene.load: %s\n", reply.c_str());
    }
    std::vector<uint8_t> capture(const std::string& mode = "deferred", int frames = 1, uint32_t w = 320, uint32_t h = 180) {
        api::RenderRequest r;
        r.mode = mode;
        r.frames = frames;
        r.spp = frames;
        r.width = w;
        r.height = h;
        const api::RenderOutcome o = host.capture(r);
        if (!o.errors.empty()) return {};
        return host.lastPixels();
    }
    bool step(int n) {
        const std::string reply = session.call("world.step", "{\"frames\":" + std::to_string(n) + "}");
        if (reply.rfind("{\"result\"", 0) == 0) return true;
        std::printf("  world.step: %s\n", reply.c_str());
        return false;
    }
    entt::entity entity(uint64_t id) const { return world.findById(id); }
    void setLayer0(uint64_t id, const char* state, uint32_t steps) {
        const entt::entity e = entity(id);
        world.registry().patch<ecs::Animator>(e, [&](ecs::Animator& a) {
            if (a.layers.empty()) a.layers.resize(1);
            a.layers[0] = ecs::AnimLayerState{state, steps, "", 0, 0, 0};
        });
    }
    render::RenderScene* scene() { return host.renderer() ? &host.renderer()->renderScene() : nullptr; }
    // The instance's own (skinned) mesh.
    render::MeshHandle meshOf(uint64_t id) {
        const auto* slot = world.registry().try_get<ecs::RenderSlot>(entity(id));
        if (!slot || !scene()) return {};
        return render::MeshHandle{scene()->instance(slot->slot).mesh, 0};
    }
};

// The rig's bind-pose vertices, as the arena's base mesh holds them.
std::vector<Vertex> baseVertices(ecs::AssetResolver& assets) {
    ecs::AssetRef ref;
    ref.path = "tests/anim/fixtures/rig.glb";
    auto m = assets.loadModel(ref, "Column");
    return m && m.value() ? m.value()->vertices : std::vector<Vertex>{};
}

struct Skinned {
    glm::vec3 position, normal;
};
Skinned cpuSkin(const Vertex& v, const std::vector<glm::mat4>& palette) {
    const float sum = v.boneWeights.x + v.boneWeights.y + v.boneWeights.z + v.boneWeights.w;
    glm::mat4 m(1.0f);
    if (sum != 0.0f) {
        m = glm::mat4(0.0f);
        for (int k = 0; k < 4; ++k) {
            const int b = std::clamp(v.boneIndices[k], 0, static_cast<int>(palette.size()) - 1);
            m += v.boneWeights[k] * palette[static_cast<std::size_t>(b)];
        }
    }
    return {glm::vec3(m * glm::vec4(v.position, 1.0f)), glm::normalize(glm::mat3(m) * v.normal)};
}

bool close(const glm::vec3& a, const glm::vec3& b, float rel = 1e-5f) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > rel * std::max(1.0f, std::abs(b[i]))) return false;
    return true;
}

// How many of `gpu`'s positions (and normals) match the CPU skin of `palette`, and how many of
// its previous positions match `prev`'s.
struct Match {
    std::size_t positions{0}, normals{0}, previous{0}, total{0};
};
Match compare(const std::vector<Vertex>& gpu, const std::vector<Vertex>& base, const std::vector<glm::mat4>& palette,
              const std::vector<glm::mat4>& prev) {
    Match m;
    m.total = std::min(gpu.size(), base.size());
    if (gpu.size() != base.size()) return m;
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        const Skinned now = cpuSkin(base[i], palette), before = cpuSkin(base[i], prev);
        m.positions += close(gpu[i].position, now.position);
        m.normals += close(gpu[i].normal, now.normal);
        m.previous += close(glm::vec3(gpu[i].boneWeights), before.position);
    }
    return m;
}

void t24_skin_matches_cpu() {
    std::printf("t24: the deformed range equals a CPU skin of the same palette\n");
    Rig rig(kRigScene);
    if (!rig.loaded || rig.capture().empty() || !rig.scene()) {
        check(false, "t24: the rig scene renders");
        return;
    }
    const auto* pose = rig.world.registry().try_get<ecs::AnimPose>(rig.entity(kRigId));
    const render::MeshHandle mesh = rig.meshOf(kRigId);
    const render::MeshInfo* info = rig.scene()->meshInfo(mesh);
    check(info && info->skinned, "t24: the rig's instance draws a skinned mesh of its own");
    if (!pose || !info) return;
    const auto gpu = rig.scene()->readMeshVertices(mesh);
    const auto base = baseVertices(rig.assets);
    const Match m = compare(gpu, base, pose->palette, pose->prevPalette);
    std::printf("  %zu vertices: positions %zu, normals %zu, previous %zu match\n", m.total, m.positions, m.normals,
                m.previous);
    check(m.total == 96 && m.positions == m.total && m.normals == m.total,
          "t24: positions and normals = the CPU skin (1e-5 relative)");
    check(m.previous == m.total, "t24: a placement: the previous positions are the pose's own");
    // The pose is not the bind pose (wave at 0.5 s).
    std::size_t moved = 0;
    for (std::size_t i = 0; i < gpu.size() && i < base.size(); ++i) moved += !close(gpu[i].position, base[i].position);
    check(moved > 48, "t24: the range holds the pose, not the bind pose");
}

void t24_two_instances_two_poses() {
    std::printf("t24: two instances of one mesh in two poses\n");
    Rig rig(twoRigScene());
    if (!rig.loaded || rig.capture().empty() || !rig.scene()) {
        check(false, "t24: the two-rig scene renders");
        return;
    }
    const auto base = baseVertices(rig.assets);
    const render::MeshHandle a = rig.meshOf(kRigId), b = rig.meshOf(0xa17a000000000005ull);
    check(a.valid() && b.valid() && a.index != b.index, "t24: each instance has its own mesh");
    const auto* pa = rig.world.registry().try_get<ecs::AnimPose>(rig.entity(kRigId));
    const auto* pb = rig.world.registry().try_get<ecs::AnimPose>(rig.entity(0xa17a000000000005ull));
    if (!pa || !pb) return;
    const auto ga = rig.scene()->readMeshVertices(a), gb = rig.scene()->readMeshVertices(b);
    const Match ma = compare(ga, base, pa->palette, pa->prevPalette), mb = compare(gb, base, pb->palette, pb->prevPalette);
    std::printf("  wave: %zu/%zu, held: %zu/%zu positions match\n", ma.positions, ma.total, mb.positions, mb.total);
    check(ma.positions == ma.total && ma.total == 96, "t24: the waving instance's range is its own pose");
    check(mb.positions == mb.total && mb.total == 96, "t24: the raised instance's range is its own pose");
    std::size_t differ = 0;
    for (std::size_t i = 0; i < ga.size() && i < gb.size(); ++i) differ += !close(ga[i].position, gb[i].position);
    check(differ > 48, "t24: the two ranges hold different shapes");
}

void t24_motion_vectors_of_a_limb() {
    std::printf("t24: the previous positions the motion vectors read\n");
    Rig rig(kRigScene);
    if (!rig.loaded || rig.capture().empty() || !rig.scene()) {
        check(false, "t24: the rig scene renders");
        return;
    }
    // A window frame: the steps, then one sync and the render. (world.step and a capture sync
    // more than once, and the previous pose, like WorldTransform::prevWorld, is the previous
    // sync's: a capture starts without motion.)
    rig.world.simulate(1.0f / 60.0f);
    rig.world.sync();
    rig.host.renderer()->render();
    const auto* pose = rig.world.registry().try_get<ecs::AnimPose>(rig.entity(kRigId));
    if (!pose) return;
    const auto gpu = rig.scene()->readMeshVertices(rig.meshOf(kRigId));
    const auto base = baseVertices(rig.assets);
    const Match m = compare(gpu, base, pose->palette, pose->prevPalette);
    std::size_t moving = 0, stillFoot = 0, foot = 0;
    for (std::size_t i = 0; i < gpu.size() && i < base.size(); ++i) {
        const float d = glm::length(gpu[i].position - glm::vec3(gpu[i].boneWeights));
        moving += d > 1e-4f;
        // The foot: the bottom face, held by the root joint alone (wave turns lower, middle, upper).
        const int bone = base[i].boneIndices.x;
        const bool rootOnly = base[i].boneWeights.x == 1.0f && bone >= 0 &&
                              bone < static_cast<int>(pose->boneToJoint.size()) &&
                              pose->boneToJoint[static_cast<std::size_t>(bone)] == 0;
        if (rootOnly) {
            ++foot;
            stillFoot += d == 0.0f;
        }
    }
    std::printf("  after one step: %zu of %zu vertices moved; the foot's %zu vertices, %zu still; previous match %zu\n",
                moving, gpu.size(), foot, stillFoot, m.previous);
    check(m.previous == m.total && m.positions == m.total && m.total == 96,
          "t24: previous positions = last frame's palette's skin, positions = this frame's");
    check(moving > 0, "t24: the swinging limb has motion");
    check(foot == 4 && stillFoot == foot, "t24: the foot (joint 0 only) has none");
}

std::size_t diffPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<uint8_t>* mask = nullptr) {
    if (a.size() != b.size()) return SIZE_MAX;
    std::size_t n = 0;
    if (mask) mask->assign(a.size() / 4, 0);
    for (std::size_t p = 0; p + 3 < a.size(); p += 4) {
        bool d = false;
        for (int c = 0; c < 3; ++c) d = d || std::abs(int(a[p + c]) - int(b[p + c])) > 8;
        n += d;
        if (mask && d) (*mask)[p / 4] = 1;
    }
    return n;
}

void t24_rt_sees_the_pose() {
    std::printf("t24: the raised arm in the G-buffer, the RT shadow and the path tracer\n");
    Rig rig(kRigScene);
    if (!rig.loaded) {
        check(false, "t24: the rig scene loads");
        return;
    }
    rig.setLayer0(kRigId, "wave", 0);   // wave at 0 s: every joint at rest (the bind pose)
    const auto bindDeferred = rig.capture("deferred", 10);
    const auto bindPt = rig.capture("rt_offline", 16);
    rig.setLayer0(kRigId, "held", 1);   // raise held at 1 s: the arm horizontal
    const auto raisedDeferred = rig.capture("deferred", 10);
    const auto raisedPt = rig.capture("rt_offline", 16);
    if (bindDeferred.empty() || raisedDeferred.empty() || bindPt.empty() || raisedPt.empty()) {
        check(false, "t24: the four captures rendered");
        return;
    }
    std::vector<uint8_t> md, mp;
    const std::size_t d = diffPixels(bindDeferred, raisedDeferred, &md), p = diffPixels(bindPt, raisedPt, &mp);
    std::size_t both = 0;
    for (std::size_t i = 0; i < md.size() && i < mp.size(); ++i) both += md[i] && mp[i];
    std::printf("  bind vs raised: deferred %zu pixels differ, path traced %zu, both %zu (of %zu)\n", d, p, both, md.size());
    check(d > 500, "t24: deferred: the raised arm changes the frame (G-buffer and its RT shadow)");
    check(p > 500, "t24: the path tracer sees the raised arm (the refit BLAS, the deformed vertices)");
    check(both > 300, "t24: the deferred frame and the path tracer change in the same place");
}

void t24_refit_and_rebuild() {
    std::printf("t24: a moving pose refits its BLAS, rebuilt every %u refits\n", render::kBlasRefitsPerRebuild);
    Rig rig(kRigScene);
    if (!rig.loaded || rig.capture("deferred", 1, 64, 36).empty() || !rig.scene()) {
        check(false, "t24: the rig scene renders");
        return;
    }
    if (!rig.scene()->rtSupported()) {
        std::printf("  (no ray tracing -- skipped)\n");
        return;
    }
    const auto start = rig.scene()->stats().skinning;
    const uint32_t frames = render::kBlasRefitsPerRebuild + 5;
    for (uint32_t i = 0; i < frames; ++i) {
        if (!rig.step(1)) break;
        (void)rig.capture("deferred", 1, 64, 36);
    }
    const auto end = rig.scene()->stats().skinning;
    const uint64_t refits = end.blasRefits - start.blasRefits, rebuilds = end.blasRebuilds - start.blasRebuilds;
    std::printf("  %u stepped frames: %llu refits, %llu rebuilds\n", frames, static_cast<unsigned long long>(refits),
                static_cast<unsigned long long>(rebuilds));
    check(refits + rebuilds >= frames, "t24: every frame with a new pose updated the BLAS");
    check(rebuilds >= 1, "t24: a rebuild after kBlasRefitsPerRebuild refits");
    check(refits >= render::kBlasRefitsPerRebuild, "t24: the frames between rebuilds refit");
    // render.stats reports it: the instance, the newest frame's skinning, the BLAS counts.
    const auto stats = rig.host.renderStats(1);
    const bool reported = stats && stats->skinning && stats->skinning->instances == 1 && stats->skinning->skinned == 1 &&
                          stats->skinning->vertices == 96 && stats->skinning->blasRefits == end.blasRefits &&
                          stats->skinning->blasRebuilds == end.blasRebuilds;
    check(reported, "t24: render.stats skinning: 1 instance, 96 vertices skinned, the refits and rebuilds");
}

void t24_unchanged_pose_skips() {
    std::printf("t24: a still pose runs no skin, bounds or refit work\n");
    Rig rig(kRigScene);
    if (!rig.loaded || rig.capture("deferred", 4, 64, 36).empty() || !rig.scene()) {
        check(false, "t24: the rig scene renders");
        return;
    }
    const auto before = rig.scene()->stats().skinning;
    (void)rig.capture("deferred", 4, 64, 36);
    const auto after = rig.scene()->stats().skinning;
    std::printf("  a still capture: skinned %u (last frame), refits %llu -> %llu\n", after.skinned,
                static_cast<unsigned long long>(before.blasRefits), static_cast<unsigned long long>(after.blasRefits));
    check(after.skinned == 0 && after.vertices == 0, "t24: no skinning on the still frames");
    check(after.blasRefits == before.blasRefits && after.blasRebuilds == before.blasRebuilds, "t24: no BLAS work");
    check(after.instances == 1, "t24: one skinned instance registered");
}

} // namespace

int main() {
    t24_skin_matches_cpu();
    t24_two_instances_two_poses();
    t24_motion_vectors_of_a_limb();
    t24_rt_sees_the_pose();
    t24_refit_and_rebuild();
    t24_unchanged_pose_skips();
    return test::finish("skinning_gpu_test", g_passed, g_failed, g_failures);
}
