#pragma once
// Loading a game module (step 3j): a shared library exporting `ohao_game_module` (game/module.hpp), loaded by the
// host executable, which exports the engine's symbols for it (ENABLE_EXPORTS), so the process has one engine.
//
// The library is read into memory and loaded from there (memfd), never from its file: the build may replace the file
// while the game runs, and the next load of the same path is a new copy beside the old one. A module is never
// unloaded: a World keeps its component pools (EnTT has no removal), whose code is the module's, and a reload swaps a
// game's behaviour onto them (game.reload).
#include "game/module.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace ohao::game {

struct LoadedModule {
    const Module* module{nullptr};
    std::filesystem::path path;   // the file it was read from
    void* handle{nullptr};        // dlopen's
    int memfd{-1};                // kept open for the process: its /proc/self/fd path stays this copy's name
};

// The module at `path`, or null with `error` set (no file, not a library, no entry, an engine symbol it needs that
// the executable does not export).
[[nodiscard]] std::shared_ptr<const LoadedModule> loadModule(const std::filesystem::path& path, std::string& error);

// A module linked into the executable (the benches link the slice statically): no file, so never reloaded.
[[nodiscard]] std::shared_ptr<const LoadedModule> linkedModule(const Module& module);

// The directory of the running executable (the default modules sit beside it).
[[nodiscard]] std::filesystem::path executableDir();

} // namespace ohao::game
