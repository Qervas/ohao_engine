// restore_render_gpu_test (sub-project 3, step 9; test 17).
//
// Drives the CLI's own stack (api::Session + host::RenderHost) on the checked-in Cornell
// scene, deferred at 960x540, one renderer per sequence, validation on:
//   1. render.capture A;
//   2. world.snapshot, then world.apply moves the metal sphere; one frame, whose instance
//      upload is the move's;
//   3. render.capture B, which must differ from A (the move is visible);
//   4. world.restore; one frame, which must upload exactly what the move did (one
//      instance back, not the table);
//   5. render.capture C, which must be bit-identical to A (post effects off and on);
//   6. world.restore again (nothing differs): it reports no change and its frame uploads
//      0 instance bytes.
// The sequence runs twice: post effects off (as cornell_box renders), then TAA, SSAO and
// bloom on, whose temporal history a later capture must also start afresh. Validation
// must report nothing. Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/scene/render_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace ohao;

namespace {

constexpr const char* kSphere = "499421b6ef3e75a9";   // MetalSphere in assets/scenes/cornell_box.scene.json

bool ok(const std::string& reply) { return reply.rfind("{\"result\":", 0) == 0; }

struct Shot {
    std::vector<uint8_t> pixels;
    std::string reply;
};

Shot capture(api::Session& s, host::RenderHost& h, const std::string& path, const std::string& post) {
    Shot shot;
    shot.reply = s.call("render.capture", "{\"path\":\"" + path +
                                              "\",\"mode\":\"deferred\",\"width\":960,\"height\":540,\"post\":" + post + "}");
    if (ok(shot.reply) && h.renderer()) {
        const auto span = h.renderer()->getPixelSpan();
        shot.pixels.assign(span.begin(), span.end());
    }
    return shot;
}

// One frame as a capture renders it; the instance bytes its RenderScene update uploaded.
std::size_t frameUpload(ecs::World& world, host::RenderHost& h) {
    world.update(0.0f);
    h.renderer()->render();
    return h.renderer()->renderScene().stats().instanceBytesUploaded;
}

std::size_t differing(const Shot& a, const Shot& b) {
    if (a.pixels.empty() || a.pixels.size() != b.pixels.size()) return SIZE_MAX;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) n += a.pixels[i] != b.pixels[i];
    return n;
}

int maxAbs(const Shot& a, const Shot& b) {
    if (a.pixels.empty() || a.pixels.size() != b.pixels.size()) return 255;
    int m = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) m = std::max(m, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
    return m;
}

struct Sequence {
    std::string loaded, moved, restored, again;
    Shot a, b, c;
    std::size_t moveBytes{SIZE_MAX}, restoreBytes{SIZE_MAX}, unchangedBytes{SIZE_MAX};
    uint64_t validation{UINT64_MAX};
};

Sequence runSequence(const reflect::TypeRegistry& types, ecs::AssetResolver& assets, const std::filesystem::path& out,
                     const std::string& tag, const std::string& post) {
    Sequence q;
    host::RenderHost h(assets);   // before the world: it must outlive it
    ecs::World world;
    api::Session s(world, types);
    s.setAssetResolver(&assets);
    h.bind(world);
    s.setRenderHandler([&h](const api::RenderRequest& r) { return h.capture(r); });

    const auto png = [&](const char* n) { return (out / (tag + "_" + n + ".png")).generic_string(); };
    q.loaded = s.call("scene.load", "{\"path\":\"assets/scenes/cornell_box.scene.json\"}");
    q.a = capture(s, h, png("a"), post);
    if (!h.renderer()) return q;
    s.call("world.snapshot", "{\"name\":\"base\"}");
    q.moved = s.call("world.apply", std::string("{\"commands\":[{\"op\":\"field.set\",\"id\":\"") + kSphere +
                                        "\",\"path\":\"/Transform/position\",\"value\":[0,-1,2]}]}");
    q.moveBytes = frameUpload(world, h);
    q.b = capture(s, h, png("b"), post);
    q.restored = s.call("world.restore", "{\"name\":\"base\"}");
    q.restoreBytes = frameUpload(world, h);
    q.c = capture(s, h, png("c"), post);
    q.again = s.call("world.restore", "{\"name\":\"base\"}");
    q.unchangedBytes = frameUpload(world, h);
    q.validation = h.renderer()->validationEnabled() ? h.renderer()->validationMessageCount() : UINT64_MAX;
    return q;
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("OHAO_VALIDATION", "1");
#else
    setenv("OHAO_VALIDATION", "1", 1);
#endif
    std::printf("restore_render_gpu_test (960x540, deferred)\n");
    const std::filesystem::path out = std::filesystem::path(OHAO_BINARY_DIR) / "restore_render";
    std::filesystem::create_directories(out);

    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);

    struct Check {
        std::string what;
        bool ok;
    };
    std::vector<Check> checks;
    const std::pair<const char*, const char*> configs[] = {{"plain", "[]"}, {"post", R"(["taa","ssao","bloom"])"}};
    for (const auto& [tag, post] : configs) {
        const Sequence q = runSequence(types, assets, out, tag, post);
        const std::size_t ab = differing(q.a, q.b), ac = differing(q.a, q.c);
        const std::string p = std::string(tag) + ": ";
        checks.push_back({p + "the scene loads and the three captures render",
                          ok(q.loaded) && !q.a.pixels.empty() && !q.b.pixels.empty() && !q.c.pixels.empty()});
        checks.push_back({p + "the move applies and is visible in the second capture",
                          ok(q.moved) && ab != 0 && ab != SIZE_MAX});
        checks.push_back({p + "the move's frame uploads instance data", q.moveBytes != 0 && q.moveBytes != SIZE_MAX});
        checks.push_back({p + "the restore changes one entity; its frame uploads what the move did",
                          ok(q.restored) && q.restored.find("\"changed\":1") != std::string::npos &&
                              q.restoreBytes == q.moveBytes});
        // Exact with post effects too since sub-project 4 step 1 (bloom's mip chain raced and
        // TAA's first frame read an unset previous view-projection; both fixed). Without the
        // history reset TAA would ghost the moved sphere.
        checks.push_back({p + "after the restore the capture is bit-identical to the first", ac == 0});
        checks.push_back({p + "an unchanged restore changes nothing and uploads 0 instance bytes",
                          ok(q.again) && q.again.find("\"created\":0,\"destroyed\":0,\"changed\":0") != std::string::npos &&
                              q.unchangedBytes == 0});
        checks.push_back({p + "validation is on and reported nothing", q.validation == 0});
        std::printf("  [%s] differing bytes: first vs moved %zu, first vs restored %zu (max abs %d)\n", tag, ab, ac,
                    maxAbs(q.a, q.c));
        std::printf("  [%s] instance bytes uploaded: move %zu, restore %zu, unchanged restore %zu; validation %llu\n",
                    tag, q.moveBytes, q.restoreBytes, q.unchangedBytes, static_cast<unsigned long long>(q.validation));
        if (!ok(q.a.reply)) std::printf("  [%s] first capture: %s\n", tag, q.a.reply.c_str());
    }

    int passed = 0, failed = 0;
    std::string failures;
    for (const Check& ch : checks) {
        std::printf("  %-78s %s\n", ch.what.c_str(), ch.ok ? "PASS" : "FAIL");
        if (ch.ok) ++passed;
        else {
            ++failed;
            failures += std::string(failures.empty() ? "" : ",") + "\"" + ch.what + "\"";
        }
    }
    std::printf("{\"suite\":\"restore_render_gpu_test\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", passed,
                failed, failures.c_str());
    return failed == 0 ? 0 : 1;
}
