// deferred_determinism_gpu_test (sub-project 4, step 1; test 1).
//
// Deferred rendering must be bit-identical run to run, post effects included: live capture
// equality and replay verification (sub-project 4) compare images byte for byte.
//
// Each configuration renders the checked-in scene through the CLI's stack (api::Session +
// host::RenderHost, render.capture at 960x540, validation on), the way `ohao run` and the
// goldens render it:
//   1. in two fresh renderers, one after the other, in this process;
//   2. in a fresh renderer in a child process (this executable, --child), so nothing a
//      process carries (a random_device seed, a reused allocation) can make them agree.
// All three must give the same bytes, and so must a second capture by the second renderer
// (RenderHost restarts temporal state between captures; live capture in sub-project 4
// relies on it). Configurations: Cornell with post effects off, each effect alone and all
// both (TAA, bloom), Cornell over 8 frames and at reference quality (test 10, RT GI's
// history and running mean), and the helmet room off and on (as its golden renders it: 30
// frames, EV100 0). Test 12's RT part (sub-project 2b, step 5): Cornell path traced,
// offline and realtime (16 samples, no denoiser), under the same three comparisons, held to
// the path tracer's BVH floor (at most 16 bytes, each off by 1) instead of bit identity; the
// validation layer must report nothing for them either.
//
// Reseeding renders validation clean. Test 12's seed part (sub-project 2b, step 4): the render
// seed moves the RT direct light and GI noise, and the same seed renders the same image again.
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "ecs/animation.hpp"
#include "ecs/combat.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/vfx.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif

using namespace ohao;
namespace fs = std::filesystem;

namespace {

struct Config {
    const char* name;
    const char* scene;
    const char* mode;
    const char* extra;   // render.capture params after mode/size
    int steps{0};        // world.step frames before the capture (effects are a function of the step)
};

constexpr Config kConfigs[] = {
    {"cornell_plain", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":[])"},
    {"cornell_taa", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":["taa"])"},
    {"cornell_bloom", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":["bloom"])"},
    {"cornell_post", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":["taa","bloom"])"},
    // Test 10 (sub-project 2b, step 8): RT GI, realtime (above: 8 frames of the denoiser's
    // history) and reference (its running mean), across renderers and processes.
    {"cornell_gi_frames8", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":[],"frames":8)"},
    {"cornell_reference", "assets/scenes/cornell_box.scene.json", "deferred", R"("post":[],"frames":4,"quality":"reference")"},
    {"helmet_plain", "assets/scenes/damaged_helmet.scene.json", "deferred", R"("post":[])"},
    {"helmet_post", "assets/scenes/damaged_helmet.scene.json", "deferred",
     R"("post":["taa","bloom"],"frames":30,"ev100":0)"},
    // Test 12, RT part (sub-project 2b, step 5): the path tracer, offline and realtime, as the
    // RT goldens render it. Compared within the BVH floor (see rtWithinFloor), not exactly.
    {"cornell_rt_offline", "assets/scenes/cornell_box.scene.json", "rt_offline", R"("spp":16,"denoise":"none")"},
    {"cornell_rt_realtime", "assets/scenes/cornell_box.scene.json", "rt_realtime", R"("spp":16,"denoise":"none")"},
    // Test 24 (sub-project 10, t24_deferred_determinism_skinned): a skinned instance (the rig
    // mid-wave): the skin pass, its bounds and the refit BLAS (RT shadows, RT GI, the tracer).
    {"anim_rig_plain", "assets/scenes/anim_rig.scene.json", "deferred", R"("post":[])"},
    {"anim_rig_post", "assets/scenes/anim_rig.scene.json", "deferred", R"("post":["taa","bloom"],"frames":30,"ev100":0)"},
    {"anim_rig_rt_offline", "assets/scenes/anim_rig.scene.json", "rt_offline", R"("spp":16,"denoise":"none")"},
    // Test 32 (sub-project 12, t32_determinism): the transparency scene (blended panes, an
    // alpha-tested card, lit soft smoke and additive sparks at a fixed step), deferred with post and
    // path traced, and the play scene 45 steps into its fight with its effects.
    {"transparency_post", "assets/scenes/transparency.scene.json", "deferred",
     R"("post":["taa","bloom"],"frames":10,"ev100":0)", 30},
    {"transparency_rt_offline", "assets/scenes/transparency.scene.json", "rt_offline", R"("spp":16,"denoise":"none")", 30},
    {"play_vfx_step45", "assets/scenes/blockout_play.scene.json", "deferred",
     R"("post":["taa","bloom"],"frames":10,"ev100":0)", 45},
};

bool isRT(const Config& c) { return std::strcmp(c.mode, "deferred") != 0; }

// The path tracer's floor: the driver builds a mesh's BVH differently run to run, and a ray
// that hits exactly on an edge two triangles share takes whichever triangle the traversal
// reaches first. Now and then a few bytes differ by 1 (measured in 2b step 5: 5 helmet renders
// in 83; Cornell none run to run, but 10 pixels between the example's scene and the same scene
// loaded from its file). A race or a scene difference moves thousands.
struct ByteDiff {
    std::size_t bytes{SIZE_MAX};
    int maxAbs{0};
};
ByteDiff byteDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    ByteDiff d;
    if (a.empty() || a.size() != b.size()) return d;
    d.bytes = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int delta = std::abs(int(a[i]) - int(b[i]));
        d.bytes += delta != 0;
        d.maxAbs = (std::max)(d.maxAbs, delta);   // parenthesized: windows.h defines max
    }
    return d;
}
bool rtWithinFloor(const ByteDiff& d) { return d.bytes <= 16 && d.maxAbs <= 1; }

std::vector<uint8_t> readBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool ok(const std::string& reply) { return reply.rfind("{\"result\":", 0) == 0; }

uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t b : bytes) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

std::string hex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

struct Image {
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> again;   // a second capture by the same renderer (when asked for)
    std::string error;            // the failing reply, if any
    uint64_t validation{UINT64_MAX};
};

std::string captureParams(const fs::path& png, const Config& c) {
    return "{\"path\":\"" + png.generic_string() + "\",\"mode\":\"" + c.mode + "\",\"width\":960,\"height\":540," +
           c.extra + "}";
}

// The frame the last capture read back.
std::vector<uint8_t> pixelsOf(host::RenderHost& h) { return h.lastPixels(); }

// One fresh renderer (RenderHost), one capture of the configuration; with `twice`, a second
// capture by the same renderer, which must render as a fresh renderer does (RenderHost
// restarts accumulation and temporal state between captures).
Image renderFresh(const reflect::TypeRegistry& types, ecs::AssetResolver& assets, const fs::path& png, const Config& c,
                  bool twice = false) {
    Image img;
    host::RenderHost h(assets);   // before the world: it must outlive it
    ecs::World world;
    api::Session s(world, types);
    s.setAssetResolver(&assets);
    // As the CLI context installs them: the play scene's fight, the rig's pose, the effects' records.
    ecs::installPhysics(world, assets);
    ecs::installCombat(world);
    ecs::installAnimation(world, assets);
    ecs::installVfx(world, assets);
    h.bind(world);
    s.setHost(&h);
    const std::string loaded = s.call("scene.load", std::string("{\"path\":\"") + c.scene + "\"}");
    if (!ok(loaded)) {
        img.error = loaded;
        return img;
    }
    if (c.steps > 0) {
        const std::string stepped = s.call("world.step", "{\"frames\":" + std::to_string(c.steps) + "}");
        if (!ok(stepped)) {
            img.error = stepped;
            return img;
        }
    }
    const std::string reply = s.call("render.capture", captureParams(png, c));
    if (!ok(reply) || !h.renderer()) {
        img.error = reply;
        return img;
    }
    img.pixels = pixelsOf(h);
    if (twice && ok(s.call("render.capture", captureParams(png, c)))) img.again = pixelsOf(h);
    img.validation = h.renderer()->validationEnabled() ? h.renderer()->validationMessageCount() : UINT64_MAX;
    return img;
}

void enableValidation() {
#ifdef _WIN32
    _putenv_s("OHAO_VALIDATION", "1");
#else
    setenv("OHAO_VALIDATION", "1", 1);
#endif
}

// --child <file>: render every configuration once and write "name hash validation" lines.
int runChild(const char* outFile, const reflect::TypeRegistry& types, ecs::AssetResolver& assets, const fs::path& dir) {
    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    for (const Config& c : kConfigs) {
        const Image img = renderFresh(types, assets, dir / (std::string("child_") + c.name + ".png"), c);
        out << c.name << ' ' << (img.pixels.empty() ? std::string("none") : hex(fnv1a(img.pixels))) << ' '
            << img.validation << '\n';
        if (isRT(c)) {   // the parent compares these within the BVH floor, not by hash
            std::ofstream raw(dir / (std::string("child_") + c.name + ".rgba"), std::ios::binary | std::ios::trunc);
            raw.write(reinterpret_cast<const char*>(img.pixels.data()), static_cast<std::streamsize>(img.pixels.size()));
        }
    }
    return out ? 0 : 1;
}

std::string selfPath(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#endif
    return argv0;
}

// Runs this executable with --child; false if it could not run or failed.
bool spawnChild(const std::string& exe, const std::string& outFile) {
#ifdef _WIN32
    const std::string quotedExe = "\"" + exe + "\"", quotedOut = "\"" + outFile + "\"";
    const intptr_t rc = _spawnl(_P_WAIT, exe.c_str(), quotedExe.c_str(), "--child", quotedOut.c_str(), nullptr);
    return rc == 0;
#else
    return std::system(("\"" + exe + "\" --child \"" + outFile + "\"").c_str()) == 0;
#endif
}

std::map<std::string, std::pair<std::string, std::string>> readChild(const std::string& file) {
    std::map<std::string, std::pair<std::string, std::string>> m;
    std::ifstream in(file);
    std::string name, hash, validation;
    while (in >> name >> hash >> validation) m[name] = {hash, validation};
    return m;
}

std::size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || a.size() != b.size()) return SIZE_MAX;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

struct Check {
    std::string what;
    bool ok;
};

} // namespace

int main(int argc, char** argv) {
    enableValidation();
    const fs::path dir = fs::path(OHAO_BINARY_DIR) / "deferred_determinism";
    fs::create_directories(dir);

    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::FileAssetResolver assets(OHAO_SOURCE_DIR);

    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return runChild(argv[2], types, assets, dir);

    std::printf("deferred_determinism_gpu_test (960x540, deferred and path traced)\n");
    std::vector<Check> checks;

    // t1: two fresh renderers in this process, then one in a child process.
    const std::string childFile = (dir / "child_hashes.txt").string();
    std::error_code ec;
    fs::remove(childFile, ec);
    const bool childRan = spawnChild(selfPath(argv[0]), childFile);
    const auto child = readChild(childFile);
    checks.push_back({"t1 the child process rendered every configuration", childRan && child.size() == std::size(kConfigs)});

    for (const Config& c : kConfigs) {
        const Image a = renderFresh(types, assets, dir / (std::string(c.name) + "_a.png"), c);
        const Image b = renderFresh(types, assets, dir / (std::string(c.name) + "_b.png"), c, /*twice=*/true);
        const std::size_t ab = differing(a.pixels, b.pixels), bb = differing(b.pixels, b.again);
        const std::string ha = a.pixels.empty() ? "none" : hex(fnv1a(a.pixels));
        const auto it = child.find(c.name);
        const std::string hc = it == child.end() ? "missing" : it->second.first;
        const std::string p = std::string(isRT(c) ? "t12 " : "t1 ") + c.name + ": ";
        checks.push_back({p + "renders", !a.pixels.empty() && !b.pixels.empty()});
        if (isRT(c)) {
            const ByteDiff dab = byteDiff(a.pixels, b.pixels), dbb = byteDiff(b.pixels, b.again);
            const ByteDiff dac = byteDiff(a.pixels, readBytes(dir / (std::string("child_") + c.name + ".rgba")));
            checks.push_back({p + "two fresh renderers in one process match within the BVH floor", rtWithinFloor(dab)});
            checks.push_back({p + "a fresh renderer in another process matches within the BVH floor", rtWithinFloor(dac)});
            checks.push_back({p + "a second capture by the same renderer matches within the BVH floor", rtWithinFloor(dbb)});
            std::printf("  [%s] differing bytes (max abs): fresh vs fresh %zu (%d), vs child %zu (%d), second capture %zu (%d)\n",
                        c.name, dab.bytes == SIZE_MAX ? std::size_t(0) : dab.bytes, dab.maxAbs,
                        dac.bytes == SIZE_MAX ? std::size_t(0) : dac.bytes, dac.maxAbs,
                        dbb.bytes == SIZE_MAX ? std::size_t(0) : dbb.bytes, dbb.maxAbs);
        } else {
            checks.push_back({p + "two fresh renderers in one process are bit-identical", ab == 0});
            checks.push_back({p + "a fresh renderer in another process is bit-identical", hc == ha});
            checks.push_back({p + "a second capture by the same renderer is bit-identical", bb == 0});
        }
        checks.push_back({p + "validation is on and reported nothing",
                          a.validation == 0 && b.validation == 0 && it != child.end() && it->second.second == "0"});
        std::printf("  [%s] differing bytes: fresh vs fresh %zu, first vs second capture %zu; hashes %s (here) %s (child)\n",
                    c.name, ab == SIZE_MAX ? std::size_t(0) : ab, bb == SIZE_MAX ? std::size_t(0) : bb, ha.c_str(),
                    hc.c_str());
        if (!a.error.empty()) std::printf("  [%s] %s\n", c.name, a.error.c_str());
    }

    // t1, t12: the render seed reaches the RT traces; reseeding renders validation clean.
    {
        const Config& c = kConfigs[std::size(kConfigs) - 1];
        host::RenderHost h(assets);
        ecs::World world;
        api::Session s(world, types);
        s.setAssetResolver(&assets);
        h.bind(world);
        s.setHost(&h);
        const bool loaded = ok(s.call("scene.load", std::string("{\"path\":\"") + c.scene + "\"}"));
        bool rendered = false;
        std::vector<uint8_t> seed0, seed7, seed0Again;
        if (loaded && ok(s.call("render.capture", captureParams(dir / "seed0.png", c))) && h.renderer()) {
            seed0 = pixelsOf(h);
            h.renderer()->setRenderSeed(7);
            rendered = ok(s.call("render.capture", captureParams(dir / "seed7.png", c)));
            seed7 = pixelsOf(h);
            h.renderer()->setRenderSeed(0);
            rendered = rendered && ok(s.call("render.capture", captureParams(dir / "seed0_again.png", c)));
            seed0Again = pixelsOf(h);
        }
        checks.push_back({"t1 captures after reseeding render, validation clean",
                          rendered && h.renderer()->validationEnabled() && h.renderer()->validationMessageCount() == 0});
        // Test 12, seed part (sub-project 2b, step 4): RT direct light and GI rays are seeded by
        // (render seed, frame index since the capture's reset, pixel, sample). Another seed
        // moves the RT noise; the same seed after it gives the first image back bit for bit.
        const bool rt = h.renderer() && h.renderer()->isRTSupported();
        const std::size_t d07 = differing(seed0, seed7), d00 = differing(seed0, seed0Again);
        std::printf("  [seed] RT %s; differing bytes seed 0 vs 7: %zu, seed 0 vs 0 again: %zu\n", rt ? "on" : "off",
                    d07 == SIZE_MAX ? std::size_t(0) : d07, d00 == SIZE_MAX ? std::size_t(0) : d00);
        checks.push_back({"t12 another render seed moves the RT shadow and GI noise", !rt || (d07 != SIZE_MAX && d07 > 0)});
        checks.push_back({"t12 the same seed again renders bit-identically", d00 == 0});
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
    std::printf("{\"suite\":\"deferred_determinism_gpu_test\",\"passed\":%d,\"failed\":%d,\"failures\":[%s]}\n", passed,
                failed, failures.c_str());
    return failed == 0 ? 0 : 1;
}
