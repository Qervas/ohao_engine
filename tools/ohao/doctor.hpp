#pragma once
// ohao doctor (sub-project 4, step 3): is this machine able to build, run and test OHAO?
// Every check is one entry {id, ok, level, detail, fix?}; `level` is what a failure of
// that check means: "error" (OHAO cannot work: exit 1) or "warn" (something degrades).
//
//   vulkan.instance    a Vulkan 1.3 instance can be created
//   vulkan.device      a physical device with Vulkan 1.3 (name, driver, API version)
//   vulkan.rt          ray tracing pipeline + acceleration structures (warn: RT modes need them)
//   vulkan.validation  VK_LAYER_KHRONOS_validation available (warn: the validation gates need it)
//   shaders.complete   every stage source under shaders/ has its SPIR-V
//   shaders.fresh      no SPIR-V older than its source or any file it #includes
//   assets.present     every asset the checked-in scenes (assets/scenes) reference exists
//   tools.glslc        glslc on PATH or in the Vulkan SDK
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

namespace ohao::doctor {

struct Options {
    bool cpuOnly{false};
    std::filesystem::path root;        // the project root (scenes, assets)
    std::filesystem::path shaderDir;   // the stage sources (default <root>/shaders)
    std::filesystem::path spirvDir;    // the SPIR-V the engine loads (default <exe dir>/bin/shaders)
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
// Stage sources under `shaderDir` (the extensions the shaders target compiles), sorted.
[[nodiscard]] std::vector<std::filesystem::path> shaderSources(const std::filesystem::path& shaderDir);
// The SPIR-V name of a source: its path relative to `shaderDir` with '/' -> '_', + ".spv".
[[nodiscard]] std::string spirvName(const std::filesystem::path& shaderDir, const std::filesystem::path& source);
// The newest modification time of `source` and every file it #includes, transitively,
// resolved as glslc does with -I<shaderDir> -I<shaderDir>/includes ("x": the including
// file's directory first). Includes that resolve nowhere are listed in `unresolved`.
[[nodiscard]] std::filesystem::file_time_type newestInput(const std::filesystem::path& shaderDir,
                                                          const std::filesystem::path& source,
                                                          std::vector<std::string>* unresolved = nullptr);

} // namespace ohao::doctor
