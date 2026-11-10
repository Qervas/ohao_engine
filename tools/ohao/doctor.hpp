#pragma once
// ohao doctor (sub-project 4, step 3): is this machine able to build, run and test OHAO?
// Every check is one entry {id, ok, level, detail, fix?}; `level` is what a failure of
// that check means: "error" (OHAO cannot work: exit 1) or "warn" (something degrades).
//
//   vulkan.instance    a Vulkan 1.3 instance can be created
//   vulkan.device      a physical device with Vulkan 1.3 (name, driver, API version)
//   vulkan.rt          ray tracing pipeline + acceleration structures (warn: RT modes need them)
//   vulkan.validation  VK_LAYER_KHRONOS_validation available (warn: the validation gates need it)
//   shaders.complete   every .slang file with [shader(...)] entry points has its SPIR-V, one per
//                      entry point (a file without one is a module)
//   shaders.fresh      no SPIR-V older than its source or any module it imports
//   assets.present     every asset the checked-in scenes (assets/scenes) reference exists
//   tools.slangc       slangc in the Vulkan SDK or on PATH (error when missing); a version other
//                      than shaders/slangc.version's fails as a warning (other SPIR-V)
//   tools.cmake        the cmake on PATH is not conda's (warn)
//   python.deps        python with numpy, Pillow and jsonschema (warn: the harnesses need them)
//   gpu.wedged         a 64x64 deferred frame renders within 2 s (after a crash the GPU can
//                      stay wedged until a reboot; this says so instead of letting 30 s
//                      renders look like regressions)
// --cpu-only skips the vulkan.* and gpu.* checks (no device is created).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ohao::game {
class Module;
}

namespace ohao::doctor {

struct Options {
    bool cpuOnly{false};
    std::filesystem::path root;        // the project root (scenes, assets)
    std::filesystem::path shaderDir;   // the stage sources (default <root>/shaders)
    std::filesystem::path spirvDir;    // the SPIR-V the engine loads (default <exe dir>/bin/shaders)
    const game::Module* game{nullptr}; // the game whose types join the engine's (--game)
};

struct Check {
    std::string id;
    bool ok{false};
    std::string level;   // "error" | "warn"
    std::string detail;
    std::string fix;     // empty when ok
};

[[nodiscard]] std::vector<Check> run(const Options& options);
// {"ok": no failing error-level check, "cpuOnly", "checks": [...]}
[[nodiscard]] std::string toJson(const std::vector<Check>& checks, bool cpuOnly);
[[nodiscard]] bool passed(const std::vector<Check>& checks);

// ---- the shader checks, exposed for tests ---------------------------------------------
// Stage sources under `shaderDir` (the extensions the shaders target compiles, and .slang
// files with an entry point; a .slang without one is a module), sorted.
[[nodiscard]] std::vector<std::filesystem::path> shaderSources(const std::filesystem::path& shaderDir);
// The entry points of a Slang source, in order, found as shaders/CMakeLists.txt finds them:
// the identifier before the first '(' after a [shader("stage")] attribute line and any
// further [...] attribute lines.
[[nodiscard]] std::vector<std::string> slangEntries(const std::string& text);
// The SPIR-V names of a source: its path relative to `shaderDir` with '/' -> '_', + ".spv";
// a .slang with several entry points gives one "<flattened>.<entry>.spv" per entry.
[[nodiscard]] std::vector<std::string> spirvNames(const std::filesystem::path& shaderDir,
                                                  const std::filesystem::path& source);
// The newest modification time of `source` and every module it imports, transitively.
// `import a.b;` resolves as slangc does with -I<shaderDir>: a/b.slang beside the importing file,
// then under shaderDir. Imports that resolve nowhere are listed in `unresolved`.
[[nodiscard]] std::filesystem::file_time_type newestInput(const std::filesystem::path& shaderDir,
                                                          const std::filesystem::path& source,
                                                          std::vector<std::string>* unresolved = nullptr);

} // namespace ohao::doctor
