#include "ohao/doctor.hpp"

#include "api/session.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "game/module.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "reflect/json_writer.hpp"

#include <vulkan/vulkan.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace ohao::doctor {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kListCap = 8;   // names listed in a detail before "+N more"

Check pass(std::string id, std::string level, std::string detail) {
    return {std::move(id), true, std::move(level), std::move(detail), {}};
}
Check fail(std::string id, std::string level, std::string detail, std::string fix) {
    return {std::move(id), false, std::move(level), std::move(detail), std::move(fix)};
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "a, b, c, +N more"
std::string listCapped(const std::vector<std::string>& items) {
    std::string s;
    for (std::size_t i = 0; i < items.size() && i < kListCap; ++i) s += (i ? ", " : "") + items[i];
    if (items.size() > kListCap) s += ", +" + std::to_string(items.size() - kListCap) + " more";
    return s;
}

fs::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(std::wstring(buf, n)).parent_path();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return fs::current_path();
}

// The first file named `name` (and `name`.exe on Windows) in the PATH directories.
fs::path findOnPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> names{name + ".exe", name};
#else
    const char sep = ':';
    const std::vector<std::string> names{name};
#endif
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, sep)) {
        if (dir.empty()) continue;
        for (const auto& n : names) {
            std::error_code ec;
            const fs::path cand = fs::path(dir) / n;
            if (fs::is_regular_file(cand, ec)) return cand;
        }
    }
    return {};
}

std::string driverVersion(uint32_t vendor, uint32_t v) {
    if (vendor == 0x10DE)   // NVIDIA packs 10.8.8.6
        return std::to_string((v >> 22) & 0x3ff) + "." + std::to_string((v >> 14) & 0xff);
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

// ---- Vulkan -------------------------------------------------------------------------------

void vulkanChecks(std::vector<Check>& out) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ohao doctor";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance);
    if (r != VK_SUCCESS) {
        out.push_back(fail("vulkan.instance", "error", "vkCreateInstance failed (" + std::to_string(r) + ")",
                           "install or repair the GPU driver and the Vulkan loader"));
        out.push_back(fail("vulkan.device", "error", "no instance", "see vulkan.instance"));
        out.push_back(fail("vulkan.rt", "warn", "no instance", "see vulkan.instance"));
    } else {
        uint32_t loader = 0;
        vkEnumerateInstanceVersion(&loader);
        out.push_back(pass("vulkan.instance", "error",
                           "loader " + std::to_string(VK_API_VERSION_MAJOR(loader)) + "." +
                               std::to_string(VK_API_VERSION_MINOR(loader)) + "." +
                               std::to_string(VK_API_VERSION_PATCH(loader))));

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        VkPhysicalDevice best = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties bestProps{};
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            const bool better = best == VK_NULL_HANDLE ||
                                (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                                 bestProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
            if (better) {
                best = d;
                bestProps = p;
            }
        }
        if (best == VK_NULL_HANDLE) {
            out.push_back(fail("vulkan.device", "error", "no physical device", "install the GPU driver"));
            out.push_back(fail("vulkan.rt", "warn", "no physical device", "see vulkan.device"));
        } else {
            const uint32_t api = bestProps.apiVersion;
            const std::string desc = std::string(bestProps.deviceName) + ", driver " +
                                     driverVersion(bestProps.vendorID, bestProps.driverVersion) + ", Vulkan " +
                                     std::to_string(VK_API_VERSION_MAJOR(api)) + "." +
                                     std::to_string(VK_API_VERSION_MINOR(api)) + "." +
                                     std::to_string(VK_API_VERSION_PATCH(api));
            if (api >= VK_API_VERSION_1_3)
                out.push_back(pass("vulkan.device", "error", desc));
            else
                out.push_back(fail("vulkan.device", "error", desc + ": OHAO needs Vulkan 1.3",
                                   "update the GPU driver"));

            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(best, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(best, nullptr, &extCount, exts.data());
            std::set<std::string> have;
            for (const auto& e : exts) have.insert(e.extensionName);
            std::vector<std::string> missing;
            for (const char* need : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                     VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
                if (!have.count(need)) missing.emplace_back(need);
            if (missing.empty())
                out.push_back(pass("vulkan.rt", "warn", "ray tracing pipeline and acceleration structures"));
            else
                out.push_back(fail("vulkan.rt", "warn", "missing " + listCapped(missing) +
                                                            ": the path tracer and RT direct light/GI are unavailable",
                                   "a GPU and driver with Vulkan ray tracing"));
        }
        vkDestroyInstance(instance, nullptr);
    }

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    const bool validation = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
        return std::string(l.layerName) == "VK_LAYER_KHRONOS_validation";
    });
    if (validation)
        out.push_back(pass("vulkan.validation", "warn", "VK_LAYER_KHRONOS_validation available"));
    else
        out.push_back(fail("vulkan.validation", "warn",
                           "VK_LAYER_KHRONOS_validation not found: the validation gates cannot run",
                           "install the Vulkan SDK"));
}

// A 64x64 deferred frame within 2 s. A wedged GPU (after a crash: device lost, or 30 s
// frames) fails here instead of looking like a regression in every render test.
void gpuCheck(std::vector<Check>& out) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    VulkanRenderer renderer(64, 64);
    if (!renderer.initialize()) {
        out.push_back(fail("gpu.wedged", "error", "the renderer did not start",
                           "if vulkan.device passes, the GPU may be wedged after a crash: reboot"));
        return;
    }
    renderer.setRenderMode(RenderMode::Deferred);
    const auto t1 = Clock::now();
    renderer.requestReadback();
    renderer.render();
    // Read back: the frame is timed to its fence, so a hung GPU cannot pass by submitting.
    const bool image = renderer.readback().size() == std::size_t{64} * 64 * 4;
    const auto t2 = Clock::now();
    const double initMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double frameMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    char detail[160];
    std::snprintf(detail, sizeof detail, "renderer start %.0f ms, 64x64 deferred frame %.1f ms", initMs, frameMs);
    renderer.shutdown();
    if (frameMs <= 2000.0 && image)
        out.push_back(pass("gpu.wedged", "error", detail));
    else
        out.push_back(fail("gpu.wedged", "error", std::string(detail) + ": over 2 s (or no image)",
                           "the GPU looks wedged after a crash: reboot"));
}

// ---- shaders ------------------------------------------------------------------------------

void shaderChecks(const Options& o, std::vector<Check>& out) {
    std::error_code ec;
    if (!fs::is_directory(o.shaderDir, ec)) {
        out.push_back(fail("shaders.complete", "error", "no shader sources at " + o.shaderDir.generic_string(),
                           "run from the project root, or pass --root"));
        out.push_back(fail("shaders.fresh", "error", "no shader sources", "see shaders.complete"));
        return;
    }
    const auto sources = shaderSources(o.shaderDir);
    std::vector<std::string> missing, stale, unresolvedAll;
    std::size_t spirvCount = 0;
    for (const fs::path& src : sources) {
        std::vector<std::string> unresolved;
        const auto newest = newestInput(o.shaderDir, src, &unresolved);
        for (auto& u : unresolved) unresolvedAll.push_back(fs::relative(src, o.shaderDir).generic_string() + ": " + u);
        for (const std::string& name : spirvNames(o.shaderDir, src)) {
            ++spirvCount;
            const fs::path spv = o.spirvDir / name;
            if (!fs::is_regular_file(spv, ec))
                missing.push_back(name);
            else if (fs::last_write_time(spv, ec) < newest)
                stale.push_back(name);
        }
    }
    const std::string where = " in " + o.spirvDir.generic_string();
    const std::string rebuild = "build the shaders target: cmake --build build --config Release --target shaders";
    if (missing.empty())
        out.push_back(pass("shaders.complete", "error", std::to_string(sources.size()) + " sources, every SPIR-V" + where));
    else
        out.push_back(fail("shaders.complete", "error",
                           std::to_string(missing.size()) + " of " + std::to_string(spirvCount) +
                               " SPIR-V missing" + where + ": " + listCapped(missing),
                           rebuild));
    if (stale.empty()) {
        std::string d = "no SPIR-V older than its source or imports";
        if (!unresolvedAll.empty()) d += "; unresolved imports: " + listCapped(unresolvedAll);
        out.push_back(pass("shaders.fresh", "error", d));
    } else {
        out.push_back(fail("shaders.fresh", "error",
                           std::to_string(stale.size()) + " SPIR-V older than its sources: " + listCapped(stale),
                           rebuild));
    }
}

// ---- assets, tools ------------------------------------------------------------------------

// Every reflected type registers without a mistake (a unit on a non-numeric field, a name taken twice, ...): the
// registry records them and nothing else reads them, and a game's reload refuses a build whose types do not register.
void typesCheck(const Options& o, std::vector<Check>& out) {
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    if (o.game) o.game->registerTypes(types);
    const std::string which = o.game ? "the engine's and " + std::string(o.game->name()) + "'s" : "the engine's";
    if (types.errors().empty())
        out.push_back(pass("types.register", "error", std::to_string(types.types().size()) + " types (" + which + ")"));
    else
        out.push_back(fail("types.register", "error", listCapped(types.errors()),
                           "fix the registration (ecs/scene_types.cpp, ecs/event_types.cpp, a game's registerTypes)"));
}

void assetCheck(const Options& o, std::vector<Check>& out) {
    const fs::path scenes = o.root / "assets" / "scenes";
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(scenes, ec))
        for (const auto& e : fs::directory_iterator(scenes, ec))
            if (e.is_regular_file() && e.path().filename().string().ends_with(".scene.json")) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        out.push_back(fail("assets.present", "warn", "no scenes in " + scenes.generic_string(),
                           "run from the project root, or pass --root"));
        return;
    }
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    ecs::FileAssetResolver assets(o.root.string());
    std::vector<std::string> missing;
    for (const fs::path& f : files) {
        ecs::World world;
        api::Session session(world, types);
        session.setAssetResolver(&assets);
        reflect::JsonWriter p;
        p.beginObject();
        p.key("path");
        p.string(fs::relative(f, o.root, ec).generic_string());
        p.key("checkAssets");
        p.boolean(true);
        p.endObject();
        const std::string reply = session.call("scene.validate", p.str());
        auto doc = ecs::parseJson(reply);
        if (!doc) continue;
        const auto& d = doc.value();
        const auto* result = d.member(d.root(), "result");
        const auto* errors = result ? d.member(*result, "errors") : nullptr;
        for (uint32_t i = 0; errors && i < errors->count; ++i) {
            const auto& e = d.child(*errors, i);
            const auto* code = d.member(e, "code");
            const auto* msg = d.member(e, "message");
            if (code && code->text == "E_ASSET_NOT_FOUND")
                missing.push_back(f.filename().string() + ": " + (msg ? std::string(msg->text) : "an asset"));
        }
    }
    if (missing.empty())
        out.push_back(pass("assets.present", "error",
                           std::to_string(files.size()) + " scenes, every referenced asset present"));
    else
        out.push_back(fail("assets.present", "error", listCapped(missing),
                           "restore the files (git checkout, or git lfs pull for large assets)"));
}

// The slangc this build compiled its shaders with (OHAO_SLANGC, from the configure step), else $VULKAN_SDK/Bin, the
// newest C:/VulkanSDK/*/Bin, then PATH. Its version against shaders/slangc.version: the pin the SPIR-V was made
// with (the CPU twins' slangc, shaders/slangc_cpp.version, is the configure step's to check).
fs::path findSlangc() {
    std::error_code ec;
#ifdef OHAO_SLANGC
    if (fs::is_regular_file(OHAO_SLANGC, ec)) return fs::path(OHAO_SLANGC);
#endif
#ifdef _WIN32
    const char* exe = "slangc.exe";
#else
    const char* exe = "slangc";
#endif
    if (const char* sdk = std::getenv("VULKAN_SDK"))
        for (const char* bin : {"Bin", "bin"})
            if (fs::is_regular_file(fs::path(sdk) / bin / exe, ec)) return fs::path(sdk) / bin / exe;
#ifdef _WIN32
    std::vector<fs::path> sdks;
    for (const auto& e : fs::directory_iterator("C:/VulkanSDK", ec))
        if (fs::is_regular_file(e.path() / "Bin" / exe, ec)) sdks.push_back(e.path() / "Bin" / exe);
    std::sort(sdks.begin(), sdks.end());
    if (!sdks.empty()) return sdks.back();
#endif
    return findOnPath("slangc");
}

std::string commandOutput(const std::string& command) {
#ifdef _WIN32
    FILE* p = _popen(command.c_str(), "r");
#else
    FILE* p = popen(command.c_str(), "r");
#endif
    if (!p) return {};
    std::string text;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) text += buf;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

void slangcCheck(const Options& o, std::vector<Check>& out) {
    const fs::path slangc = findSlangc();
    if (slangc.empty()) {
        out.push_back(fail("tools.slangc", "error", "slangc not in $VULKAN_SDK, C:/VulkanSDK nor on PATH",
                           "unpack Slang's own release at shaders/slangc.version's version into "
                           "~/.local/opt/slang-<version> and configure again"));
        return;
    }
#ifdef _WIN32   // cmd /c strips one outer pair of quotes
    const std::string version = commandOutput("\"\"" + slangc.string() + "\" -v 2>&1\"");
#else
    const std::string version = commandOutput("\"" + slangc.string() + "\" -v 2>&1");
#endif
    std::string pinned;
    std::ifstream pin(o.shaderDir / "slangc.version");
    std::getline(pin, pinned);
    while (!pinned.empty() && std::isspace(static_cast<unsigned char>(pinned.back()))) pinned.pop_back();
    const std::string detail = slangc.generic_string() + " (" + (version.empty() ? "no version" : version) + ")";
    if (!pinned.empty() && version.rfind(pinned, 0) != 0)
        out.push_back(fail("tools.slangc", "warn",
                           detail + ", not the pinned " + pinned + ": the SPIR-V may differ from the goldens' build",
                           "configure with -DSLANGC naming Slang " + pinned +
                               "'s slangc (shaders/slangc.version; on Linux ~/.local/opt/slang-" + pinned +
                               "/bin/slangc)"));
    else
        out.push_back(pass("tools.slangc", "error", detail));
}

void toolChecks(const Options& o, std::vector<Check>& out) {
    slangcCheck(o, out);

    const fs::path cmake = findOnPath("cmake");
    if (cmake.empty())
        out.push_back(fail("tools.cmake", "warn", "no cmake on PATH",
                           "install CMake 3.21+, or set OHAO_CMAKE for tools/check.py"));
    else if (lower(cmake.generic_string()).find("conda") != std::string::npos)
        out.push_back(fail("tools.cmake", "warn", cmake.generic_string() + " is conda's cmake, first on PATH",
                           "call the real cmake by its path (the scoop shim here) or set OHAO_CMAKE; "
                           "tools/check.py refuses conda's"));
    else
        out.push_back(pass("tools.cmake", "warn", cmake.generic_string()));
    // The harnesses' Python modules are checked by tools/check.py, on the interpreter CTest runs them with (the
    // configured Python3_EXECUTABLE): `python` on PATH is often another one.
}

} // namespace

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

} // namespace

std::vector<std::string> slangEntries(const std::string& text) {
    std::vector<std::string> entries;
    std::size_t at = 0;
    while ((at = text.find("[shader(\"", at)) != std::string::npos) {
        std::size_t p = text.find(")]", at);
        if (p == std::string::npos) break;
        p += 2;
        // Further attribute lines: [numthreads(...)], [outputtopology(...)], ...
        for (;;) {
            while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
            if (p >= text.size() || text[p] != '[') break;
            const std::size_t eol = text.find('\n', p);
            p = eol == std::string::npos ? text.size() : eol;
        }
        // The identifier before the first '(': the function's name.
        const std::size_t paren = text.find('(', p);
        if (paren == std::string::npos) break;
        std::size_t end = paren;
        while (end > p && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
        std::size_t begin = end;
        while (begin > p && isIdent(text[begin - 1])) --begin;
        if (begin < end) entries.push_back(text.substr(begin, end - begin));
        at = paren;
    }
    return entries;
}

std::vector<fs::path> shaderSources(const fs::path& shaderDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(shaderDir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".slang") continue;
        if (!slangEntries(readAll(e.path())).empty()) out.push_back(e.path());   // else a module
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> spirvNames(const fs::path& shaderDir, const fs::path& source) {
    std::string rel = fs::relative(source, shaderDir).generic_string();
    std::replace(rel.begin(), rel.end(), '/', '_');
    const auto entries = slangEntries(readAll(source));
    if (entries.size() == 1) return {rel + ".spv"};
    std::vector<std::string> names;
    for (const auto& e : entries) names.push_back(rel + "." + e + ".spv");
    return names;
}

fs::file_time_type newestInput(const fs::path& shaderDir, const fs::path& source, std::vector<std::string>* unresolved) {
    std::error_code ec;
    fs::file_time_type newest = fs::file_time_type::min();
    std::set<fs::path> seen;
    std::vector<fs::path> work{fs::weakly_canonical(source, ec)};
    while (!work.empty()) {
        const fs::path file = work.back();
        work.pop_back();
        if (!seen.insert(file).second) continue;
        newest = std::max(newest, fs::last_write_time(file, ec));
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t hash = line.find_first_not_of(" \t");
            if (hash == std::string::npos) continue;
            if (line.compare(hash, 7, "import ") != 0) continue;   // import a.b;  ->  a/b.slang
            const std::size_t semi = line.find(';', hash);
            if (semi == std::string::npos) continue;
            std::string mod = line.substr(hash + 7, semi - hash - 7);
            mod.erase(std::remove_if(mod.begin(), mod.end(), [](unsigned char c) { return std::isspace(c); }),
                      mod.end());
            std::replace(mod.begin(), mod.end(), '.', '/');
            const std::string name = mod + ".slang";
            bool found = false;
            for (const fs::path& c : {file.parent_path() / name, shaderDir / name}) {
                if (fs::is_regular_file(c, ec)) {
                    work.push_back(fs::weakly_canonical(c, ec));
                    found = true;
                    break;
                }
            }
            if (!found && unresolved) unresolved->push_back(name);
        }
    }
    return newest;
}

std::vector<Check> run(const Options& options) {
    Options o = options;
    if (o.root.empty()) o.root = fs::current_path();
    if (o.shaderDir.empty()) o.shaderDir = o.root / "shaders";
    if (o.spirvDir.empty()) o.spirvDir = exeDir() / "bin" / "shaders";
    std::vector<Check> out;
    if (!o.cpuOnly) vulkanChecks(out);
    shaderChecks(o, out);
    typesCheck(o, out);
    assetCheck(o, out);
    toolChecks(o, out);
    if (!o.cpuOnly) gpuCheck(out);
    return out;
}

bool passed(const std::vector<Check>& checks) {
    return std::none_of(checks.begin(), checks.end(), [](const Check& c) { return !c.ok && c.level == "error"; });
}

std::string toJson(const std::vector<Check>& checks, bool cpuOnly) {
    reflect::JsonWriter w;
    w.beginObject();
    w.key("ok");
    w.boolean(passed(checks));
    w.key("cpuOnly");
    w.boolean(cpuOnly);
    w.key("checks");
    w.beginArray();
    for (const Check& c : checks) {
        w.beginObject();
        w.key("id");
        w.string(c.id);
        w.key("ok");
        w.boolean(c.ok);
        w.key("level");
        w.string(c.level);
        w.key("detail");
        w.string(c.detail);
        if (!c.ok && !c.fix.empty()) {
            w.key("fix");
            w.string(c.fix);
        }
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.take();
}

} // namespace ohao::doctor
