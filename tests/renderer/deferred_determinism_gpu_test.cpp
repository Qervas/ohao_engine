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
// three (TAA, SSAO, bloom), and the helmet room off and on (as its golden renders it: 30
// frames, exposure 1).
//
// The SSAO noise is a function of the render seed alone (pinned for seed 0), and
// setRenderSeed reaches the SSAO pass; reseeding renders validation clean.
//
// Exit code 0 = pass; the last line is a JSON summary.
#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "ohao/render_host.hpp"
#include "render/deferred/deferred_renderer.hpp"
#include "render/deferred/post_processing_pipeline.hpp"
#include "render/deferred/ssao_pass.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    const char* extra;   // render.capture params after mode/size
};

constexpr Config kConfigs[] = {
    {"cornell_plain", "assets/scenes/cornell_box.scene.json", R"("post":[])"},
    {"cornell_taa", "assets/scenes/cornell_box.scene.json", R"("post":["taa"])"},
    {"cornell_ssao", "assets/scenes/cornell_box.scene.json", R"("post":["ssao"])"},
    {"cornell_bloom", "assets/scenes/cornell_box.scene.json", R"("post":["bloom"])"},
    {"cornell_post", "assets/scenes/cornell_box.scene.json", R"("post":["taa","ssao","bloom"])"},
    {"helmet_plain", "assets/scenes/damaged_helmet.scene.json", R"("post":[])"},
    {"helmet_post", "assets/scenes/damaged_helmet.scene.json",
     R"("post":["taa","ssao","bloom"],"frames":30,"exposure":1)"},
};

// FNV-1a 64 of SSAOPass::noise(0)'s bytes. The generator is integer hashing and exact float
// arithmetic, so this holds on every compiler and machine; a change to it shows here.
constexpr const char* kSeed0NoiseHash = "63fe119f530d173f";

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
    return "{\"path\":\"" + png.generic_string() + "\",\"mode\":\"deferred\",\"width\":960,\"height\":540," + c.extra + "}";
}

std::vector<uint8_t> pixelsOf(host::RenderHost& h) {
    const auto span = h.renderer()->getPixelSpan();
    return {span.begin(), span.end()};
}

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
    h.bind(world);
    s.setRenderHandler([&h](const api::RenderRequest& r) { return h.capture(r); });
    const std::string loaded = s.call("scene.load", std::string("{\"path\":\"") + c.scene + "\"}");
    if (!ok(loaded)) {
        img.error = loaded;
        return img;
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

    std::printf("deferred_determinism_gpu_test (960x540, deferred)\n");
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
        const std::string p = std::string("t1 ") + c.name + ": ";
        checks.push_back({p + "renders", !a.pixels.empty() && !b.pixels.empty()});
        checks.push_back({p + "two fresh renderers in one process are bit-identical", ab == 0});
        checks.push_back({p + "a fresh renderer in another process is bit-identical", hc == ha});
        checks.push_back({p + "a second capture by the same renderer is bit-identical", bb == 0});
        checks.push_back({p + "validation is on and reported nothing",
                          a.validation == 0 && b.validation == 0 && it != child.end() && it->second.second == "0"});
        std::printf("  [%s] differing bytes: fresh vs fresh %zu, first vs second capture %zu; hashes %s (here) %s (child)\n",
                    c.name, ab == SIZE_MAX ? std::size_t(0) : ab, bb == SIZE_MAX ? std::size_t(0) : bb, ha.c_str(),
                    hc.c_str());
        if (!a.error.empty()) std::printf("  [%s] %s\n", c.name, a.error.c_str());
    }

    // t1: the SSAO noise is a function of the render seed alone, and the seed reaches the
    // pass. (Not checked through the image: deferred_lighting.frag never samples the SSAO
    // output, so SSAO does not reach any pixel today; see the step 1 change record.)
    {
        const auto bytesOf = [](const std::array<glm::vec4, 16>& n) {
            std::vector<uint8_t> b(sizeof(n));
            std::memcpy(b.data(), n.data(), sizeof(n));
            return b;
        };
        const auto n0 = SSAOPass::noise(0), n7 = SSAOPass::noise(7);
        bool unit = true;
        for (const auto* n : {&n0, &n7})
            for (const glm::vec4& v : *n) unit = unit && std::abs(v.x * v.x + v.y * v.y - 1.0f) < 1e-5f && v.z == 0.0f && v.w == 0.0f;
        const std::string h0 = hex(fnv1a(bytesOf(n0)));
        checks.push_back({"t1 the SSAO noise for a seed is the same every time", bytesOf(SSAOPass::noise(0)) == bytesOf(n0)});
        checks.push_back({"t1 the SSAO noise for seed 0 is the pinned one (" + h0 + ")", h0 == kSeed0NoiseHash});
        checks.push_back({"t1 another seed gives other noise", bytesOf(n7) != bytesOf(n0)});
        checks.push_back({"t1 the noise vectors are unit vectors in XY", unit});

        const Config& c = kConfigs[std::size(kConfigs) - 1];   // helmet_post: SSAO on
        host::RenderHost h(assets);
        ecs::World world;
        api::Session s(world, types);
        s.setAssetResolver(&assets);
        h.bind(world);
        s.setRenderHandler([&h](const api::RenderRequest& r) { return h.capture(r); });
        const bool loaded = ok(s.call("scene.load", std::string("{\"path\":\"") + c.scene + "\"}"));
        uint32_t initial = UINT32_MAX, after7 = UINT32_MAX, after0 = UINT32_MAX;
        bool rendered = false;
        if (loaded && ok(s.call("render.capture", captureParams(dir / "seed0.png", c))) && h.renderer()) {
            const auto* pp = h.renderer()->getDeferredRenderer()->getPostProcessing();
            initial = pp->noiseSeed();
            h.renderer()->setRenderSeed(7);
            after7 = pp->noiseSeed();
            rendered = ok(s.call("render.capture", captureParams(dir / "seed7.png", c)));
            h.renderer()->setRenderSeed(0);
            after0 = pp->noiseSeed();
            rendered = rendered && ok(s.call("render.capture", captureParams(dir / "seed0_again.png", c)));
        }
        checks.push_back({"t1 a fresh renderer's SSAO noise uses seed 0", initial == 0});
        checks.push_back({"t1 setRenderSeed reaches the SSAO pass (7, then 0)", after7 == 7 && after0 == 0});
        checks.push_back({"t1 captures after reseeding render, validation clean",
                          rendered && h.renderer()->validationEnabled() && h.renderer()->validationMessageCount() == 0});
        std::printf("  [seed] noise(0) hash %s; SSAO seed initially %u, after setRenderSeed(7) %u, after (0) %u\n",
                    h0.c_str(), initial, after7, after0);
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
