#pragma once
// Loading a game module (step 3j): a shared library exporting `ohao_game_module` (game/module.hpp), loaded by the
// host executable, which exports the engine's symbols for it, so the process has one engine. On Linux the executable
// exports all of them (ENABLE_EXPORTS) and dlopen binds the module. On Windows the executable exports what the games
// import (tools/gen_game_imports.py), the module imports them from "ohao_engine.exe", delay-loaded, and its hook
// (game/module_imports_win.cpp) answers that name with the executable that loaded it; ohao_game_bind binds them all
// at load.
//
// The library is loaded from a copy, never from its file (Linux: read into a memfd; Windows: copied beside it, into
// <module>.loaded/): the build may replace the file while the game runs, and the next load of the same path is a new
// copy beside the old one. A module is never unloaded: a World keeps its component pools (EnTT has no removal), whose code is the module's, and a reload swaps a
// game's behaviour onto them (game.reload).
#include "game/module.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace ohao::game {

struct LoadedModule {
    const Module* module{nullptr};
    std::filesystem::path path;   // the file it was read from
    void* handle{nullptr};        // dlopen's; LoadLibrary's on Windows
    int memfd{-1};                // Linux: kept open for the process: its /proc/self/fd path stays this copy's name
};

// The module at `path`, or null with `error` set (no file, not a library, no entry, an engine symbol it needs that
// the executable does not export).
[[nodiscard]] std::shared_ptr<const LoadedModule> loadModule(const std::filesystem::path& path, std::string& error);

// A module linked into the executable (the benches link the slice statically): no file, so never reloaded.
[[nodiscard]] std::shared_ptr<const LoadedModule> linkedModule(const Module& module);

// The directory of the running executable (the default modules sit beside it).
[[nodiscard]] std::filesystem::path executableDir();

// The module a game's build puts beside the executables: libohao_<game>.so; ohao_<game>.dll on Windows.
[[nodiscard]] std::filesystem::path builtModule(const std::string& game);

} // namespace ohao::game
